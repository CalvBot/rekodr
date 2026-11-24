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

// The Apps tabs: not sorted yet, recorded (as a game, an app or chat), never recorded, and the folder rules.
enum AppsFilter : i32 { FILTER_NEW, FILTER_INCLUDED, FILTER_IGNORED, FILTER_FOLDERS };

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
// Start with Windows: a "clipback" value under HKCU\...\Run.

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

// Each listed monitor's place on the desktop, by output index, for the popups' monitor map
static RECT g_monitor_rect[16];

static void options_list_monitors()
{
    OptionsUi &o = g_app.options;
    o.monitor_count = 0;

    // GDI device name (\\.\DISPLAY1) -> monitor name
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
            g_monitor_rect[o.monitor_count] = d.DesktopCoordinates;
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

static const f32 OPT_PAD = 16;     // a card's padding left and right
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

static void opt_card_begin()
{
    g_card = {};
    g_card.a = ImGui::GetCursorScreenPos();
    g_card.w = ImGui::GetContentRegionAvail().x;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(g_card.a.x, g_card.a.y + dp(4)));
}

static void opt_card_end(f32 space_after = 12)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 y = ImGui::GetCursorScreenPos().y + dp(4);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(g_card.a, ImVec2(g_card.a.x + g_card.w, y), col32(COL_PANEL), dp(10));
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(g_card.a.x, y));
    ImGui::Dummy(ImVec2(g_card.w, dp(space_after) - ImGui::GetStyle().ItemSpacing.y));
}

// Opens a folder in Explorer (made first if it isn't there yet).
static void opt_open_dir(Path dir)
{
    CreateDirectoryW(dir.s, 0);
    ShellExecuteW(0, L"open", dir.s, 0, 0, SW_SHOWNORMAL);
    log_line("options: opened a cache folder");
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
    g_row.h = h > 0 ? h : dp(sub ? 52.f : 44.f);
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

// The control column of a slider row: more than half the row, so a slider is never short.
static f32 opt_wide_w() { return floorf(fmaxf(dp(320), (g_card.w - 2 * dp(OPT_PAD)) * 0.56f)); }

// A slider row: the name and line on the left; on the right the value in words over a slider as wide as the
// control column. Returns true while it changes; opt_row_slider_end draws the value (after the change, so it's this
// frame's) and closes the row. *done is set when the knob is let go, the moment to save.
static bool opt_row_slider(const char *name, const char *sub, const char *id, f32 *v, f32 lo, f32 hi, bool *done)
{
    f32 cw = opt_wide_w();
    opt_row_begin(name, sub, cw, dp(68));
    f32 x = g_row.x1 - cw, y = g_row.y + dp(12) + g_font->FontSize + dp(6);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    bool changed = lib_slider(id, v, lo, hi, cw, dp(24));
    *done = ImGui::IsItemDeactivated();
    return changed;
}

static void opt_row_slider_end(const char *value)
{
    f32 cw = opt_wide_w();
    opt_text_at(ImGui::GetWindowDrawList(), g_font, ImVec2(g_row.x1 - cw + kit_px(7), g_row.y + dp(12)), COL_TEXT, value,
                g_row.x1);
    opt_row_end();
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
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(COL_PANEL2));
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
        ImGui::PushStyleColor(ImGuiCol_Header, rgb(0x2a2a2a));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, rgb(0x313131));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, rgb(0x313131));
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
// The wheel over a closed one steps its choice only on the list pages, which don't scroll (screen_options). On a
// page that scrolls, the first drop-down to pass under the mouse would take the wheel: the page stops and the
// choice changes.
static bool g_combo_wheel;

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
    bool changed = opt_combo(id, items, notes, count, index, width, disabled, preview);
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
        dl->AddRectFilled(ImVec2(x, p.y + dp(2)), ImVec2(x + w, p.y + h), col32(COL_BG), dp(6)); // the cap's edge
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + w, p.y + h - dp(2)), col32(0x2a2a2a), dp(6));
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

// A footer line: `left` dim on the left, and  <  2 / 5  >  on the right, all fixed sizes. The mouse wheel over the
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
    kit_next_tip("Previous page");
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
    kit_next_tip("Next page");
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
    if (hot) ImGui::GetWindowDrawList()->AddRectFilled(a, z, col32(0x1d1d1d), dp(6));
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
    if (!us) { snprintf(out, cap, "-"); return; } // -: not since clipback started
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
static const u32 CATEGORY_COLOURS[] = {COL_WARN, KIND_LOOK[KIND_APP].rgb, KIND_LOOK[KIND_GAME].rgb,
                                       KIND_LOOK[KIND_CHAT].rgb, 0x969696};

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
    if (app_folder_only(a)) return false; // its folder's row stands for it (one per person in a call)
    AppCategory cat = app_category(g_app.config, a);
    switch (filter) {
    case FILTER_NEW: if (cat != CAT_NONE) return false; break;
    case FILTER_INCLUDED: if (cat == CAT_NONE || cat == CAT_IGNORED) return false; break;
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

// Chips for a Clip saved popup choice, from `first` (TOAST_CLIP_DEFAULT for a game's "Same as all", else
// TOAST_CLIP_FULL) to TOAST_CLIP_OFF, each as wide as its label, from the cursor. True when one was picked.
static const char *const TOAST_CLIP_CHIPS[] = {"Same as all", "Full", "Small", "Off"};
static f32 toast_clip_chips_w(i32 first)
{
    f32 w = 0;
    for (i32 i = first; i <= TOAST_CLIP_OFF; i++) w += text_w(g_font, TOAST_CLIP_CHIPS[i]) + 2 * kit_px(14) + (i > first ? dp(6) : 0);
    return w;
}
static ImTextureID toast_preview(bool pill, f32 scale, const char *name, i32 *w, i32 *h); // toast.cpp

// While Full or Small (`style`) is under the mouse, its popup floats by the chip (item min a, max b): under it, or
// over it where there's no room, at three quarters of the window's size. A picture, so the Tooltips switch leaves it.
static void toast_clip_preview(ToastClip style, ImVec2 a, ImVec2 b)
{
    char name[256], file[300];
    save_file_name(g_app.audio.game.s, "", now_us(), name, sizeof name); // as a save of the game in front would be
    snprintf(file, sizeof file, "%s.mp4", name);
    f32 s = dp(0.75f);
    i32 w, h;
    ImTextureID tex = toast_preview(style == TOAST_CLIP_SMALL, s, file, &w, &h);
    if (!tex) return;
    ImVec2 vp = ImGui::GetMainViewport()->Pos, vs = ImGui::GetMainViewport()->Size;
    f32 gap = dp(8), x = fminf(a.x, vp.x + vs.x - w - gap), y = b.y + gap;
    if (y + h > vp.y + vs.y - gap) y = a.y - gap - h;
    x = floorf(fmaxf(x, vp.x + gap)), y = floorf(y);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 p0(x, y), p1(x + w, y + h);
    if (style == TOAST_CLIP_SMALL) dl->AddImage(tex, p0, p1);
    else {
        f32 r = floorf(TOAST_ROUND * s);
        dl->AddImageRounded(tex, p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
        dl->AddRect(p0, p1, col32(COL_LINE), r, 0, 1);
    }
}

static bool toast_clip_chips(const char *id, i32 first, ToastClip *v)
{
    bool changed = false;
    for (i32 i = first; i <= TOAST_CLIP_OFF; i++) {
        if (i > first) ImGui::SameLine(0, dp(6));
        char label[48];
        snprintf(label, sizeof label, "%s##%s%d", TOAST_CLIP_CHIPS[i], id, i);
        bool clicked = kit_chip(label, *v == i, 0, text_w(g_font, TOAST_CLIP_CHIPS[i]) + 2 * kit_px(14));
        if ((i == TOAST_CLIP_FULL || i == TOAST_CLIP_SMALL) && ImGui::IsItemHovered())
            toast_clip_preview((ToastClip)i, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (clicked && *v != i) {
            *v = (ToastClip)i;
            changed = true;
        }
    }
    return changed;
}

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
    else if (mh && height >= mh) snprintf(out, cap, "%dp - records %dp", height, mh);
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
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(COL_PANEL2));
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

    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(COL_HOVER));
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
        snprintf(def, sizeof def, "Default - %s", rl);
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
    snprintf(def, sizeof def, "Default - %s", PRESET_LABELS[preset_step(c.preset.s)]);
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
    snprintf(def, sizeof def, "Default - %s", RC_LABELS[rc_index(c.rate_control.s)]);
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
    opt_unit("0-" "51");
    ImGui::EndDisabled();

    game_line("Capture window", x);
    {
        static const char *const wnotes[] = {0, "this game's window, not the whole screen", "the whole screen"};
        snprintf(def, sizeof def, "Default - %s", c.window_capture ? "On" : "Off");
        const char *witems[] = {def, "On", "Off"};
        i32 wsel = r.window > 0 ? 1 : r.window < 0 ? 2 : 0;
        if (opt_combo("##window", witems, wnotes, 3, &wsel, cw)) {
            r.window = wsel == 1 ? 1 : wsel == 2 ? -1 : 0;
            log_line("options: %s capture window: %s", a.exe.s, wsel == 1 ? "on" : wsel == 2 ? "off" : "default");
            options_apply(APPLY_RECORDER | APPLY_AUDIO);
        }
    }

    game_line("Sessions", x);
    i32 m = a.session_mode;
    if (opt_combo("##mode", SESSION_LABELS, 0, COUNT(SESSION_LABELS), &m, cw)) {
        a.session_mode = (SessionMode)m;
        log_line("options: %s sessions: %s", a.exe.s, session_mode_name(a.session_mode));
        options_apply(0);
    }

    // Clip saved over this game: its chips under the label, as they don't fit the column
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Clip saved popup");
    if (toast_clip_chips("gameclip", TOAST_CLIP_DEFAULT, &a.toast_clip)) {
        log_line("options: %s clip saved popup: %s", a.exe.s, a.toast_clip ? toast_clip_name(a.toast_clip) : "same as all");
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
    bool custom = r.bitrate_mbps || r.fps || r.height || r.preset.s[0] || r.rate_control.s[0] || r.cq || r.window;
    ImGui::SetCursorPosX(x + cw - 2 * bw - dp(8));
    if (kit_button("Reset", KIT_GHOST, ImVec2(bw, 0), custom)) {
        r = {};
        log_line("options: %s records with the default settings", a.exe.s);
        options_apply(APPLY_RECORDER | APPLY_AUDIO);
    }
    ImGui::SameLine(0, dp(8));
    if (kit_button("Done", KIT_PRIMARY, ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

// What a game records with, if it differs: "60 Mbps - 120 fps".
static void game_summary(const AppInfo &a, char *out, i32 cap)
{
    const RecOverride &r = a.rec;
    out[0] = 0;
    i32 n = 0;
    auto add = [&](const char *t) { n += snprintf(out + n, cap - n, "%s%s", n ? " - " : "", t); };
    char t[32];
    if (r.bitrate_mbps) { snprintf(t, sizeof t, "%g Mbps", r.bitrate_mbps); add(t); }
    if (r.height) { if (r.height < 0) add("native"); else { snprintf(t, sizeof t, "%dp", r.height); add(t); } }
    if (r.fps) { snprintf(t, sizeof t, "%d fps", r.fps); add(t); }
    if (r.preset.s[0]) add(PRESET_LABELS[preset_step(r.preset.s)]);
    if (r.rate_control.s[0]) add(RC_LABELS[rc_index(r.rate_control.s)]);
    if (r.window) add(r.window > 0 ? "its window" : "whole screen");
    if (a.session_mode == SESSION_ALWAYS) add("sessions saved");
    else if (a.session_mode == SESSION_NEVER) add("no sessions");
    if (a.toast_clip == TOAST_CLIP_FULL) add("full popup");
    else if (a.toast_clip == TOAST_CLIP_SMALL) add("small popup");
    else if (a.toast_clip == TOAST_CLIP_OFF) add("no popup");
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
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(COL_PANEL2));
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
    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(COL_HOVER));
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

// Renaming an app (the right-click menu): the name it gets in files and on tracks. It starts as the app's name, or
// its exe's, all selected so typing replaces it. What is typed is looked for in the games list and on the art service;
// picking a game names the app that.
static void apps_rename_popup()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    ImGui::SetNextWindowSize(ImVec2(kit_px(480), 0));
    lib_popup_style_push(18);
    bool open = ImGui::BeginPopupModal("rename app", 0, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    lib_popup_style_pop();
    if (!open) return;
    if (o.menu_app < 0 || o.menu_app >= c.app_count) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    AppInfo &a = c.apps[o.menu_app];
    char title[160];
    snprintf(title, sizeof title, "Name for %s", a.exe.s);
    kit_heading(title);

    static f64 changed_at;
    if (ImGui::IsWindowAppearing()) {
        if (a.name.s[0]) str_set(o.edit, a.name.s);
        else {
            snprintf(o.edit, 64, "%s", a.exe.s);
            size_t n = strlen(o.edit);
            if (n > 4 && !_stricmp(o.edit + n - 4, ".exe")) o.edit[n - 4] = 0;
        }
        changed_at = ImGui::GetTime();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(COL_HOVER));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(dp(12), dp(10)));
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputText("##name", o.edit, 64, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemEdited()) changed_at = ImGui::GetTime();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // The games: the games list's, which answer each key at once, then the art service's below them, asked once the
    // text stays the same a moment and shown only while they are for the text in the box. A name that is what was
    // typed goes first among the list's. Six rows, always, and the line under them, so the buttons stay where they are.
    const i32 ROWS = 6;
    static char local[ROWS][128], online[8][128], names[ROWS][128];
    static i32 online_years[8], years[ROWS];
    char q[64];
    str_set(q, o.edit);
    char *tq = trim(q);
    i32 nl = gamedb_find_local(tq, local, ROWS);
    bool settled = ImGui::GetTime() - changed_at > 0.15;
    i32 no = *tq && settled ? gamedb_find_online(tq, online, online_years, 8) : *tq ? -1 : 0;
    char want[128], letters[128];
    gdb_letters(tq, want, sizeof want);
    i32 n = 0;
    for (i32 pass = 0; pass < 3; pass++) // 0: the list's names that are what was typed; 1: the list's; 2: the art service's
        for (i32 i = pass < 2 ? 0 : nl; i < (pass < 2 ? nl : nl + (no > 0 ? no : 0)) && n < ROWS; i++) {
            const char *name = i < nl ? local[i] : online[i - nl];
            gdb_letters(name, letters, sizeof letters);
            if (pass == 0 && strcmp(letters, want)) continue;
            bool dup = false;
            for (i32 d = 0; d < n && !dup; d++) {
                char seen[128];
                gdb_letters(names[d], seen, sizeof seen);
                dup = !strcmp(seen, letters);
            }
            if (dup) continue;
            str_set(names[n], name);
            years[n++] = i < nl ? 0 : online_years[i - nl];
        }

    ImGui::Dummy(ImVec2(0, dp(2)));
    bool picked = false;
    f32 w = ImGui::GetContentRegionAvail().x, rh = dp(46), is = dp(32);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 l0 = ImGui::GetCursorScreenPos();
    for (i32 i = 0; i < n; i++) {
        ImVec2 r0(l0.x, l0.y + i * rh);
        ImGui::SetCursorScreenPos(r0);
        ImGui::PushID(i);
        if (ImGui::Selectable("##pick", false, 0, ImVec2(w, rh - dp(2)))) {
            snprintf(o.edit, 64, "%s", names[i]);
            picked = true;
        }
        ImGui::PopID();
        ImVec2 i0(r0.x + dp(8), floorf(r0.y + (rh - dp(2) - is) / 2));
        ID3D11ShaderResourceView *icon = game_db_icon(names[i]);
        if (icon) dl->AddImageRounded((ImTextureID)icon, i0, ImVec2(i0.x + is, i0.y + is), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, dp(6));
        else dl->AddRectFilled(i0, ImVec2(i0.x + is, i0.y + is), col32(COL_HOVER), dp(6));
        f32 right = r0.x + w - dp(10);
        if (years[i]) {
            char y[8];
            snprintf(y, sizeof y, "%d", years[i]);
            ImGui::PushFont(g_font_small);
            f32 yw = ImGui::CalcTextSize(y).x;
            ImGui::PopFont();
            right -= yw;
            opt_text_at(dl, g_font_small, ImVec2(right, floorf(r0.y + (rh - dp(2) - g_font_small->FontSize) / 2)), COL_FAINT, y, right + yw + 1);
            right -= dp(12);
        }
        opt_text_at(dl, g_font, ImVec2(i0.x + is + dp(12), floorf(r0.y + (rh - dp(2) - g_font->FontSize) / 2)), COL_TEXT, names[i], right);
    }
    ImGui::SetCursorScreenPos(ImVec2(l0.x, l0.y + ROWS * rh));
    const char *status = !o.edit[0] ? "Type a name, or pick the game it is"
                       : no < 0    ? "Searching..."
                       : !n        ? "No game has that name; it is kept as typed"
                                   : " ";
    ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_FAINT));
    ImGui::TextUnformatted(status);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    i32 b = lib_dialog_buttons(ICON_RENAME "  Save", KIT_PRIMARY);
    if (b == -1 || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    else if (b == 1 || enter || picked) {
        char hint[64];
        if (a.desc.s[0]) str_set(hint, a.desc.s); // what it shows as with no name: its File description
        else default_app_name(a.exe.s, hint, sizeof hint);
        char name[64];
        str_set(name, o.edit);
        for (char *p = name; *p; p++) if (*p == '|') *p = ' '; // | separates fields in settings.txt
        str_set(a.name.s, trim(name));
        log_line("options: %s is shown as \"%s\"", a.exe.s, a.name.s[0] ? a.name.s : hint);
        if (a.name.s[0]) gamedb_confirm(a, a.name.s);
        sessions_game_renamed(a.exe.s);
        options_apply(APPLY_AUDIO);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// The controls on an app row: an arrow that moves it between Included and Excluded, then the three categories an
// included app can have, the app's own one filled in its colour. All of it keeps its place whatever is chosen.
static const AppCategory ROW_CATS[] = {CAT_GAME, CAT_APP, CAT_COMMUNICATION};
static f32 cat_pill_w() { return dp(84); }
static f32 cat_pills_w() { return ImGui::GetFrameHeight() + dp(14) + COUNT(ROW_CATS) * cat_pill_w() + (COUNT(ROW_CATS) - 1) * dp(4); }

// An arrow pointing the way the app would go along the tabs (right to Excluded, left back to Included), then the
// categories. `own` is filled; `inherited` (a folder's rule, when there's no own one) is only outlined. Moving it
// back makes it an App; picking a category of an excluded one includes it too.
static bool cat_pills_draw(AppCategory own, AppCategory inherited, f32 x, f32 y, AppCategory *picked)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 w = cat_pill_w(), h = ImGui::GetFrameHeight(), gap = dp(4);
    bool changed = false;
    AppCategory effective = own != CAT_NONE ? own : inherited;
    bool included = effective != CAT_IGNORED;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    kit_next_tip(included ? "Move to Excluded" : "Move to Included");
    if (kit_icon_button(included ? ICON_FORWARD : ICON_BACK, "##move", KIT_GHOST)) {
        *picked = included ? CAT_IGNORED : CAT_APP;
        changed = true;
    }
    x += h + dp(14);
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
                    on || via ? col : hot ? COL_TEXT : included ? COL_DIM : 0x4d4d4d, label, p.x + w);
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
    // One that now belongs under another tab goes there at once
    OptionsUi &o = g_app.options;
    if (!app_matches(*a, o.filter, o.search)) o.order_dirty = true;
    return true;
}

static bool pick_folder(const Path &start, Path *out);

// Editing a folder rule's path, where a * can stand for one folder of any name (VoiceStreams\*\clips-off).
static void folder_edit_popup()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(18), dp(16)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(COL_PANEL2));
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
    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(COL_HOVER));
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
        snprintf(line, sizeof line, "%s - %d app%s", f.path, apps, apps == 1 ? "" : "s");
        opt_text_at(dl, g_font_small, ImVec2(text_x, ty + nh + dp(3)), COL_DIM, line, text_max);

        AppCategory picked;
        if (cat_pills_draw(f.category, CAT_NONE, pills_x, cy, &picked)) folder_set(f.path, picked);

        ImGui::SetCursorScreenPos(ImVec2(more_x, cy));
        kit_next_tip("More options. Right-click the row too");
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

    // Tabs with their counts (each as wide as the widest can be, so none moves); search and Add on the right
    static const char *tabs[] = {"New", "Included", "Excluded", "Folders"};
    i32 counts[4] = {}, folder_only = 0;
    for (i32 i = 0; i < c.app_count; i++) {
        AppCategory cat = app_category(c, c.apps[i]);
        if (app_folder_only(c.apps[i])) {
            folder_only++;
            continue;
        }
        counts[cat == CAT_NONE ? FILTER_NEW : cat == CAT_IGNORED ? FILTER_IGNORED : FILTER_INCLUDED]++;
    }
    counts[FILTER_FOLDERS] = c.folder_count;
    bool folders = o.filter == FILTER_FOLDERS;
    f32 fh = ImGui::GetFrameHeight();
    f32 search_w = dp(220), add_w = dp(118), tab_w = text_w(g_font, "Excluded") + text_w(g_font, "999") + kit_px(36);
    ImVec2 filter_at = ImGui::GetCursorScreenPos();
    for (i32 i = 0; i < COUNT(tabs); i++) {
        char id[32], n[16];
        snprintf(id, sizeof id, "%s##tab%d", tabs[i], i);
        snprintf(n, sizeof n, "%d", counts[i]);
        if (i) ImGui::SameLine(0, dp(6));
        if (kit_chip(id, o.filter == i, n, tab_w) && o.filter != i) {
            o.filter = i;
            o.order_dirty = true;
            o.apps_page = 0;
            o.folders_page = 0;
        }
    }
    f32 filter_w = COUNT(tabs) * tab_w + (COUNT(tabs) - 1) * dp(6);
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

    f32 rh = dp(50);
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
                else snprintf(msg, sizeof msg, "%s", o.filter == FILTER_NEW      ? "Nothing new. Apps show up here when they play sound"
                                                     : o.filter == FILTER_INCLUDED ? "Nothing recorded yet. Pick Game, App or Chat on a new app"
                                                                                   : "Nothing excluded. An app's arrow moves it here");
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
        if (!playing && !a.last_heard_us) when[0] = 0; // added by hand, not heard yet
        snprintf(line, sizeof line, "%s%s%s%s%s", a.exe.s, when[0] ? " - " : "", when, summary[0] ? " - " : "", summary);
        opt_text_at(dl, g_font_small, ImVec2(text_x, ty + nh + dp(3)), playing ? COL_OK : COL_DIM, line, text_max);

        cat_pills(&a, pills_x, cy);

        ImGui::SetCursorScreenPos(ImVec2(more_x, cy));
        kit_next_tip("More options. Right-click the row too");
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
        if (folder_only && o.filter != FILTER_NEW)
            snprintf(left, sizeof left, "%d app%s, %d more under Folders", o.order_count, o.order_count == 1 ? "" : "s", folder_only);
        else snprintf(left, sizeof left, "%d app%s", o.order_count, o.order_count == 1 ? "" : "s");
        opt_pager("apps", &o.apps_page, o.order_count, rows, left);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hotkeys

static const char *ACTION_LABELS[] = {"Save the last", "Open the window", "Open the timeline", "Recording on / off",
                                      "Stop recording", "Start recording"};
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

// "save 90s" -> 90 and seconds; "save 2m" -> 2 and minutes.
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

    f32 fh = ImGui::GetFrameHeight(), rh = dp(46), head_h = dp(30);
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

        // The keys: caps in a fixed slot; click to press new ones ("Press keys..." in the same slot)
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
                        "Press keys...", keys_x + keys_w);
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
        // Only a save has a length; the other actions leave the column empty (it keeps its place).
        if (save) {
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
        }

        u32 mods, vk;
        bool same = false;
        for (i32 k = 0; k < c.hotkey_count; k++) same |= k != i && h->keys.s[0] && !_stricmp(c.hotkeys[k].keys.s, h->keys.s);
        const char *warn = !capturing && !hotkey_parse(h->keys.s, &mods, &vk) ? "Not a key combination"
                           : same ? "Same keys as another hotkey" : 0;
        if (warn) opt_text_at(dl, g_font, ImVec2(warn_x, opt_row_text_y(g_font)), COL_WARN, warn, del_x - dp(8));

        if (hot && !o.capture_slot) {
            ImGui::SetCursorScreenPos(ImVec2(del_x, cy));
            kit_next_tip("Remove");
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
    if (is_test_copy()) snprintf(left, sizeof left, "Test copy: the real " APP_NAME " keeps the hotkeys");
    else snprintf(left, sizeof left, "%d hotkey%s", c.hotkey_count, c.hotkey_count == 1 ? "" : "s");
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
    snprintf(out, cap, "%dx%d, %d Hz%s", m.width, m.height, m.hz, m.primary ? ", main" : "");
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

// Monitor, resolution and frame rate: Options > Recording and the first-run setup.
static void opt_rows_video(f32 wide)
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    char preview[160], note[64];
    snprintf(preview, sizeof preview, "%d. not connected", c.output_idx + 1);
    if (c.output_idx >= 0 && c.output_idx < o.monitor_count) {
        monitor_label(c.output_idx, preview, sizeof preview);
        monitor_note(c.output_idx, note, sizeof note);
        size_t n = strlen(preview);
        snprintf(preview + n, sizeof preview - n, " - %s", note);
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
}

// How much the buffer keeps, by length or by size: Options > Recording and the first-run setup. The other is
// shown as an estimate at the game bitrate (the highest a game records at, if one has its own).
static void opt_rows_buffer()
{
    Config &c = g_app.config;
    static const char *limits[] = {"Length", "Size"};
    i32 by = c.buffer_by_size ? 1 : 0;
    opt_row_begin("Limit the buffer by", 0, 2 * dp(98) + kit_px(6));
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
        if (rate > c.bitrate_mbps) snprintf(line, sizeof line, "Up to about %s, at a game's own %g Mbps", est, rate);
        else snprintf(line, sizeof line, "Up to about %s at %g Mbps", est, rate);
        if (opt_row_f32("Keep the last", line, "##hours", &c.buffer_hours, 0.1, 24 * 14, "hours")) {
            log_line("options: buffer keeps %g h", c.buffer_hours);
            options_apply(APPLY_RECORDER);
        }
    } else {
        f64 h = c.buffer_gb * 1e9 / bytes_per_hour;
        snprintf(line, sizeof line, "At least %.0f h %02.0f min at %s%g Mbps", floor(h), fmod(h * 60, 60),
                 rate > c.bitrate_mbps ? "a game's own " : "", rate);
        if (opt_row_f32("Keep up to", line, "##gb", &c.buffer_gb, 1, 100000, "GB")) {
            log_line("options: buffer keeps %g GB", c.buffer_gb);
            options_apply(APPLY_RECORDER);
        }
    }
}

static void options_recording()
{
    Config &c = g_app.config;
    const Recorder &rec = g_app.recorder;
    f32 wide = dp(320);

    opt_section("VIDEO");
    opt_card_begin();
    opt_rows_video(wide);
    if (opt_row_toggle("Show the mouse pointer", 0, "##mouse", &c.draw_mouse)) {
        log_line("options: mouse %s", c.draw_mouse ? "shown" : "hidden");
        options_apply(APPLY_RECORDER);
    }
    if (opt_row_toggle("Capture game windows", "Records the window of the game you are in, not the whole screen. Each game can set its own.",
                       "##wincap", &c.window_capture)) {
        log_line("options: capture window %s by default", c.window_capture ? "on" : "off");
        options_apply(APPLY_RECORDER | APPLY_AUDIO);
    }
    {
        static const char *keys[] = {"auto", "wgc", "dxgi"};
        static const char *items[] = {"Automatic", "Graphics Capture", "Desktop Duplication"};
        static const char *notes[] = {0, "Windows 10 1903 and later", "the older way; can freeze"};
        i32 sel = 0;
        for (i32 i = 0; i < COUNT(keys); i++)
            if (!strcmp(c.capture.s, keys[i])) sel = i;
        char cap_preview[64], sub[64];
        const char *used = rec.wgc ? "Graphics Capture" : "Desktop Duplication";
        if (sel == 0 && rec.recording) snprintf(cap_preview, sizeof cap_preview, "Automatic - %s", used);
        else snprintf(cap_preview, sizeof cap_preview, "%s", items[sel]);
        snprintf(sub, sizeof sub, "%s", rec.recording ? (rec.wgc ? "Recording with Graphics Capture" : "Recording with Desktop Duplication") : "How the screen is read");
        if (opt_row_combo("Capture method", sub, "##capture", items, notes, COUNT(items), &sel, wide, 0, cap_preview)) {
            str_set(c.capture.s, keys[sel]);
            log_line("options: capture %s", c.capture.s);
            options_apply(APPLY_RECORDER);
        }
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
    notes[0] = !rec.encoders_known ? "testing..." : first < 0 ? "none works" : VENDOR_LABELS[first];
    u32 no_vendor = 0;
    for (i32 v = 0; v < VENDOR_COUNT; v++) {
        bool ok = !rec.encoders_known || (works & (1 << v));
        items[v + 1] = VENDOR_LABELS[v];
        notes[v + 1] = !ok ? "doesn't work here" : v == VENDOR_CPU ? "costs game frames" : 0;
        if (!ok) no_vendor |= 1u << (v + 1);
    }
    i32 enc = vendor_parse(c.encoder.s) + 1;
    char enc_preview[96];
    if (enc == 0) snprintf(enc_preview, sizeof enc_preview, "Automatic - %s", notes[0]);
    else snprintf(enc_preview, sizeof enc_preview, "%s", items[enc]);
    char status[160];
    if (rec.paused) snprintf(status, sizeof status, "Off");
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
    if (opt_row_f32("Peak bitrate", "Times the bitrate, 1 to 4. Not with CBR", "##peak", &c.peak_factor, 1, 4, "x")) {
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
    // ffmpeg options of the player's own, added after clipback's; a typo shows as "Not recording" on Encoder
    {
        f32 tw = fminf(dp(360), g_card.w * 0.5f);
        opt_row_begin("Custom encoder options", "ffmpeg options added after " APP_NAME "'s, such as -tune ll", tw);
        char args[sizeof c.encoder_args];
        if (opt_text("##encargs", c.encoder_args, tw, 0, "None", args, sizeof args) && strcmp(trim(args), c.encoder_args)) {
            str_set(c.encoder_args, trim(args));
            log_line("options: custom encoder options \"%s\"", c.encoder_args);
            options_apply(APPLY_RECORDER);
        }
        opt_row_end();
    }
    opt_card_end();

    opt_section("BUFFER");
    opt_card_begin();
    opt_rows_buffer();
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
    if (opt_text("##ffmpeg", ff, wide, 0, "Found by itself", text, sizeof text)) {
        char *t = trim(text);
        utf8_to_wide(t, c.ffmpeg.s, MAX_PATH);
        log_line("options: ffmpeg %s", t[0] ? t : "found by itself");
        options_apply(APPLY_RECORDER);
    }
    opt_row_end();
    opt_card_end();
}

// ---------------------------------------------------------------------------------------------------------------
// Integrations: other programs that work with clipback. Stream Deck keys (streamdeck\ in the repo, talking over the
// pipe) and the video editors a clip opens in.

static void open_url(const char *url); // setup.cpp
static void update_start(); // update.cpp
static void update_stop();  // update.cpp
static const char OPT_API_REPO_URL[] = "https://github.com/CalvBot/clipback-api"; // the art service's code
static const char OPT_APP_REPO_URL[] = "https://github.com/CalvBot/clipback";
// Where Support goes (a donation page). The button is hidden while it's empty.
// TODO: the Ko-fi (or similar) page once there is one.
static const char OPT_SUPPORT_URL[] = "";
static const char OPT_SGDB_KEY_URL[] = "https://www.steamgriddb.com/profile/preferences/api";

static const wchar_t SD_PLUGIN[] = L"dev.calvbot.clipback.sdPlugin";

static bool path_is_file(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Stream Deck's exe, from where its installer puts it.
static bool streamdeck_exe(Path *out)
{
    wchar_t pf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) return false;
    swprintf(out->s, COUNT(out->s), L"%s\\Elgato\\StreamDeck\\StreamDeck.exe", pf);
    return path_is_file(out->s);
}

// Where Stream Deck loads plugins from, and the clipback plugin in it.
static Path streamdeck_plugin_dest()
{
    Path p = {};
    wchar_t ad[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH))
        swprintf(p.s, COUNT(p.s), L"%s\\Elgato\\StreamDeck\\Plugins\\%s", ad, SD_PLUGIN);
    return p;
}

// The plugin that comes with clipback: streamdeck\ next to the exe, or in the source tree (app\build\clipback.exe).
static bool streamdeck_plugin_source(Path *out)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(0, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;
    const wchar_t *tries[] = {L"%s\\streamdeck\\%s\\manifest.json", L"%s\\..\\..\\streamdeck\\%s\\manifest.json"};
    for (const wchar_t *t : tries) {
        swprintf(out->s, COUNT(out->s), t, exe, SD_PLUGIN);
        if (path_is_file(out->s)) {
            *wcsrchr(out->s, L'\\') = 0;
            wchar_t full[MAX_PATH];
            if (GetFullPathNameW(out->s, MAX_PATH, full, 0)) wcscpy_s(out->s, full);
            return true;
        }
    }
    return false;
}

static DWORD process_find(const wchar_t *exe_name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e = {sizeof e};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !pid; ok = Process32NextW(snap, &e))
        if (!_wcsicmp(e.szExeFile, exe_name)) pid = e.th32ProcessID;
    CloseHandle(snap);
    return pid;
}

// Copies the plugin into Stream Deck's plugins (replacing an older copy; a link to the source tree is left alone)
// and restarts Stream Deck so it loads it. Stream Deck ignores a polite close, so it's ended.
static bool streamdeck_install(char *msg, i32 cap)
{
    Path src, sd;
    if (!streamdeck_plugin_source(&src)) {
        snprintf(msg, cap, "The plugin isn't next to clipback.exe");
        return false;
    }
    if (!streamdeck_exe(&sd)) {
        snprintf(msg, cap, "Install Stream Deck first");
        return false;
    }
    Path dest = streamdeck_plugin_dest();
    DWORD attr = GetFileAttributesW(dest.s);
    bool linked = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
    if (DWORD pid = process_find(L"StreamDeck.exe")) {
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 5000);
            CloseHandle(h);
        }
    }
    if (!linked) {
        // SHFileOperation wants double-0-terminated lists; copying the folder into Plugins makes Plugins\<id>
        wchar_t from[MAX_PATH + 2] = {}, to[MAX_PATH + 2] = {};
        wcscpy_s(from, src.s);
        wcscpy_s(to, dest.s);
        *wcsrchr(to, L'\\') = 0;
        CreateDirectoryW(to, 0);
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_COPY;
        op.pFrom = from;
        op.pTo = to;
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT;
        if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) {
            snprintf(msg, cap, "Couldn't copy the plugin into Stream Deck's folder");
            log_line("integrations: copying the Stream Deck plugin failed");
            return false;
        }
    }
    ShellExecuteW(0, L"open", sd.s, 0, 0, SW_SHOWMINNOACTIVE);
    log_line("integrations: Stream Deck plugin %s, Stream Deck restarted", linked ? "linked already" : "installed");
    snprintf(msg, cap, "Installed. Stream Deck restarted: drag the " APP_NAME " keys from its list");
    return true;
}

// The newest Premiere Pro in Program Files\Adobe, or DaVinci Resolve where Blackmagic puts it.
static bool editor_find(bool premiere, Path *out)
{
    wchar_t pf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) return false;
    if (!premiere) {
        swprintf(out->s, COUNT(out->s), L"%s\\Blackmagic Design\\DaVinci Resolve\\Resolve.exe", pf);
        return path_is_file(out->s);
    }
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%s\\Adobe\\Adobe Premiere Pro*", pf);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    wchar_t best[MAX_PATH] = L"";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        wchar_t exe[MAX_PATH];
        swprintf(exe, MAX_PATH, L"%s\\Adobe\\%s\\Adobe Premiere Pro.exe", pf, fd.cFileName);
        if (path_is_file(exe) && wcscmp(fd.cFileName, best) > 0) { // "...2026" after "...2025"
            wcscpy_s(best, fd.cFileName);
            wcscpy_s(out->s, exe);
            found = true;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// The editor's exe: the one set in Options, else the one found.
static bool editor_exe(bool premiere, Path *out)
{
    const Path &set = premiere ? g_app.config.premiere : g_app.config.resolve;
    if (set.s[0] && path_is_file(set.s)) {
        *out = set;
        return true;
    }
    return editor_find(premiere, out);
}

// A file of one type ("Programs", "*.exe"), starting in start's folder. False when none was picked.
static bool pick_file(const Path &start, Path *out, const wchar_t *kind, const wchar_t *spec)
{
    IFileOpenDialog *d = 0;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
    COMDLG_FILTERSPEC types[] = {{kind, spec}};
    d->SetFileTypes(1, types);
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (start.s[0]) {
        Path dir = start;
        if (wchar_t *slash = wcsrchr(dir.s, L'\\')) *slash = 0;
        IShellItem *folder = 0;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir.s, 0, IID_PPV_ARGS(&folder)))) {
            d->SetFolder(folder);
            folder->Release();
        }
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
    ImGui::GetIO().AddMouseButtonEvent(0, false); // the dialog took the button-up
    return ok;
}

// A status line on the right of a row: a dot in the status colour and the text.
static void opt_status(const char *text, u32 col, f32 right, f32 left)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 y = opt_row_text_y(g_font), tw = text_w(g_font, text), r = dp(4);
    f32 x = fmaxf(left + 2 * r + dp(8), right - tw);
    dl->AddCircleFilled(ImVec2(x - dp(8) - r, y + g_font->FontSize / 2), r, col32(col));
    opt_text_at(dl, g_font, ImVec2(x, y), col == COL_DIM ? COL_DIM : COL_TEXT, text, right);
}

// Sharing: links to clips (r2.cpp). With no setup they go to the free public hosts and the player's uploaders (ShareX
// .sxcu files) in link_hosts; with the player's
// own Cloudflare R2 bucket they go there. The secret key goes to Windows' Credential Manager, never settings.txt.
// The budget picks the most a month may cost; the bucket holds all that buys (r2_limit_for_budget), and the oldest
// unpinned links are removed before an upload that wouldn't fit, or when a lower budget is saved.
// The budget is US dollars a month, what Cloudflare bills in. The slider runs from $0 (the free 10 GB) to
// R2_BUDGET_TOP (about 10 TB) on a cubic curve, so the low end isn't cramped, in steps of $0.05; typed amounts go to
// the cent, up to R2_BUDGET_MOST.
static const f64 R2_BUDGET_TOP = 150, R2_BUDGET_MOST = 10000;

// "$" where Windows' currency is US dollars, else "US$", so it's never read as another dollar
static const char *opt_usd()
{
    static char sign[8];
    if (!sign[0]) {
        wchar_t iso[9] = L"";
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SINTLSYMBOL, iso, COUNT(iso));
        str_set(sign, !wcscmp(iso, L"USD") ? "$" : "US$");
    }
    return sign;
}

// The budget's label: the amount, then the space it buys. "Free: 10 GB", "$4.50 a month: 310 GB".
static void opt_budget_label(f64 dollars, char *out, i32 cap)
{
    f64 keep = (f64)r2_limit_for_budget(dollars) / (f64)R2_GB;
    const char *unit = "GB";
    if (keep >= 1000) keep /= 1000, unit = "TB";
    char size[24];
    if (fabs(keep - floor(keep + 0.5)) < 0.05) snprintf(size, sizeof size, "%.0f %s", keep, unit);
    else snprintf(size, sizeof size, "%.1f %s", keep, unit);
    if (dollars <= 0) snprintf(out, cap, "Free: %s", size);
    else snprintf(out, cap, "%s%.2f a month: %s", opt_usd(), dollars, size);
}

// A count with thousands commas: 1,204
static void fmt_count(i64 n, char *out, i32 cap)
{
    char d[32];
    i32 len = snprintf(d, sizeof d, "%lld", n < 0 ? 0 : n), o = 0;
    for (i32 i = 0; i < len && o < cap - 1; i++) {
        if (i && (len - i) % 3 == 0 && o < cap - 2) out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

// A row's name and line drawn by hand, for rows whose name is bold or whose line has its own colour and a dot.
static void opt_row_lines(const char *name, ImFont *nf, const char *sub, u32 sub_col, bool dot, f32 max_x)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 nh = nf->FontSize, sh = g_font_small->FontSize, gap = dp(3);
    f32 ty = floorf(g_row.y + (g_row.h - nh - gap - sh) / 2), x = g_row.x0;
    opt_text_at(dl, nf, ImVec2(x, ty), COL_TEXT, name, max_x);
    if (dot) {
        f32 r = dp(3);
        dl->AddCircleFilled(ImVec2(x + r, ty + nh + gap + sh / 2), r, col32(sub_col));
        x += 2 * r + dp(6);
    }
    opt_text_at(dl, g_font_small, ImVec2(x, ty + nh + gap), dot && sub_col != COL_DIM ? COL_TEXT : sub_col, sub, max_x);
}

// The bars in the Space and Reads rows: across the control column, with a small value in a slot on their right.
static const f32 OPT_BAR_H = 14, OPT_BAR_TEXT_W = 120;

static void opt_bar_frame(ImVec2 *a, ImVec2 *z)
{
    f32 cw = opt_wide_w(), w = cw - dp(OPT_BAR_TEXT_W) - dp(10), h = dp(OPT_BAR_H);
    *a = ImVec2(g_row.x1 - cw, floorf(g_row.y + (g_row.h - h) / 2));
    *z = ImVec2(a->x + w, a->y + h);
    ImGui::GetWindowDrawList()->AddRectFilled(*a, *z, col32(COL_PANEL2), dp(3));
}

static void opt_bar_value(ImVec2 z, const char *text, u32 col)
{
    f32 h = dp(OPT_BAR_H);
    opt_text_at(ImGui::GetWindowDrawList(), g_font_small,
                ImVec2(z.x + dp(10), floorf(z.y - h + (h - g_font_small->FontSize) / 2)), col, text, g_row.x1);
}

// What's in the bucket, oldest first, for the Space row: each link, whether it goes at the limit being looked at
// (`now`: a draft budget below what the links take) and whether it goes next, to make room for a clip of the max
// file size (`next`). Read again when the history or the limit changes, and every 2 s.
struct OptSpace {
    R2Share s[R2_MAX_SHARES];
    u8 now[R2_MAX_SHARES], next[R2_MAX_SHARES];
    i32 n, going;
    i64 other, total, limit;
    u32 gen;
    f64 at;
};
static OptSpace g_space;

static void opt_space_read(i64 limit)
{
    OptSpace &v = g_space;
    f64 t = ImGui::GetTime();
    u32 gen = r2_history_gen();
    if (v.at > 0 && v.gen == gen && v.limit == limit && t - v.at < 2) return;
    v.gen = gen;
    v.limit = limit;
    v.at = t;
    static u8 with[R2_MAX_SHARES];
    v.n = r2_evict_preview(limit, 0, v.now, v.s, R2_MAX_SHARES);
    r2_evict_preview(limit, (i64)(g_app.config.share_max_mb * 1e6), with, v.s, R2_MAX_SHARES);
    v.other = r2_untracked_bytes();
    v.total = v.other;
    v.going = 0;
    for (i32 i = 0; i < v.n; i++) {
        v.next[i] = with[i] && !v.now[i];
        v.going += v.now[i];
        v.total += v.s[i].bytes;
    }
}

// The bucket as a bar of `limit`: a block per link, sized by its bytes, the oldest on the left. Pinned ones have a
// light top edge, the ones that go next are amber, and the ones a draft budget would remove are red and struck
// through. Other files in the bucket are grey, at the end. The line under the name says what's under the mouse.
static void opt_space_row(i64 limit, bool draft)
{
    OptSpace &v = g_space;
    bool ready = r2_ready();
    if (ready) opt_space_read(limit);
    opt_row_begin(0, 0, opt_wide_w(), dp(52));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a, z;
    opt_bar_frame(&a, &z);
    f32 w = z.x - a.x;
    i64 scale = ready && v.total > limit ? v.total : limit > 0 ? limit : 1;
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##space", ImVec2(w, z.y - a.y));
    bool over = ImGui::IsItemHovered();
    f32 mx = ImGui::GetIO().MousePos.x, mid = floorf((a.y + z.y) / 2);
    i32 hot = -1; // a link's index, or -2 for the other files
    if (ready) {
        dl->PushClipRect(a, z, true);
        f64 x = a.x;
        for (i32 i = 0; i <= v.n; i++) {
            i64 bytes = i < v.n ? v.s[i].bytes : v.other;
            if (bytes <= 0) continue;
            f32 x0 = (f32)x, x1 = (f32)(x + (f64)w * bytes / scale);
            x += (f64)w * bytes / scale;
            if (over && mx >= x0 && mx < x1) hot = i < v.n ? i : -2;
            bool pinned = i < v.n && v.s[i].pinned, now = i < v.n && v.now[i], next = i < v.n && v.next[i];
            u32 col = i == v.n ? COL_FAINT : now ? COL_RED : next ? COL_WARN : pinned ? 0x9fc4ff : COL_BLUE;
            f32 x1g = x1 - (x1 - x0 > dp(3) ? kit_px(1) : 0);
            bool lit = hot == (i < v.n ? i : -2);
            dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1g, z.y), col32(col, lit ? 1.f : 0.8f));
            if (pinned) dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1g, a.y + dp(3)), col32(0xffffff, 0.85f));
            if (now) dl->AddLine(ImVec2(x0, mid), ImVec2(x1g, mid), col32(COL_BG), dp(2));
        }
        // Where the limit cuts the bar, when the links are over it
        if (scale > limit) {
            f32 lx = floorf(a.x + w * (f32)((f64)limit / scale));
            dl->AddLine(ImVec2(lx, a.y - dp(3)), ImVec2(lx, z.y + dp(3)), col32(COL_TEXT), dp(2));
        }
        dl->PopClipRect();
    }
    char used[32], lim[32], t[80], sub[320];
    u32 sub_col = COL_DIM;
    lib_r2_size(ready ? v.total : 0, used, sizeof used);
    lib_r2_size(limit, lim, sizeof lim);
    snprintf(t, sizeof t, "%s of %s", used, lim);
    opt_bar_value(z, ready ? t : "Not set up", COL_DIM);
    R2Usage u = r2_usage();
    if (hot >= 0) {
        const R2Share &h = v.s[hot];
        char size[32];
        lib_r2_size(h.bytes, size, sizeof size);
        snprintf(sub, sizeof sub, "%s - %s%s", h.title[0] ? h.title : h.game[0] ? h.game : "Clip", size,
                 v.now[hot] ? ", removed on Save" : v.next[hot] ? ", goes next" : h.pinned ? ", pinned" : "");
        sub_col = COL_TEXT;
    } else if (hot == -2) {
        char size[32];
        lib_r2_size(v.other, size, sizeof size);
        snprintf(sub, sizeof sub, "Other files - %s", size);
        sub_col = COL_TEXT;
    } else if (ready && draft && v.going) {
        snprintf(sub, sizeof sub, "Save removes %d link%s", v.going, v.going == 1 ? "" : "s");
        sub_col = COL_RED;
    } else if (ready && u.class_a >= 0) snprintf(sub, sizeof sub, "About %s%.2f this month", opt_usd(), u.month_cost);
    else sub[0] = 0;
    opt_row_lines("Space", g_font, sub, sub_col, false, a.x - dp(20));
    opt_row_end();
}

// The account's reads this month as a bar of r2_read_cap (r2.cpp pauses the links near it)
static void opt_reads_row()
{
    Config &c = g_app.config;
    R2Usage u = r2_usage();
    bool known = r2_ready() && u.account_reads >= 0;
    opt_row_begin("Reads this month", u.capped ? "Links are paused until the 1st" : 0, opt_wide_w(), dp(52));
    ImVec2 a, z;
    opt_bar_frame(&a, &z);
    char got[24], cap[24], t[64];
    if (known && c.r2_read_cap > 0) {
        f32 f = fminf(1.f, (f32)((f64)u.account_reads / c.r2_read_cap));
        u32 col = u.capped ? COL_RED : f > 0.8f ? COL_WARN : COL_BLUE;
        if (f > 0) ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + fmaxf(dp(3), (z.x - a.x) * f), z.y), col32(col), dp(3));
    }
    if (!r2_ready()) str_set(t, "Not set up");
    else if (u.account_reads < 0) str_set(t, "Not read yet");
    else {
        r2_fmt_reads(u.account_reads, got, sizeof got);
        r2_fmt_reads(c.r2_read_cap, cap, sizeof cap);
        if (c.r2_read_cap > 0) snprintf(t, sizeof t, "%s of %s", got, cap);
        else snprintf(t, sizeof t, "%s, no limit", got);
    }
    opt_bar_value(z, t, COL_DIM);
    opt_row_end();
}

// The budget being picked, -1 for none: a draft the Space bar previews until Save writes it to r2_budget and the
// bucket is cleaned up to it, or the undo button drops it.
static f32 g_budget_draft = -1;

// The budget: a slider (R2_BUDGET_TOP above), a box for an exact amount, then Save and undo, all in fixed slots.
static void opt_budget_row()
{
    Config &c = g_app.config;
    f32 &draft = g_budget_draft;
    if (draft >= 0 && fabsf(draft - c.r2_budget) < 0.004f && !ImGui::IsAnyItemActive()) draft = -1;
    f64 shown = draft >= 0 ? draft : c.r2_budget;
    bool dirty = draft >= 0 && fabsf(draft - c.r2_budget) >= 0.004f;
    char label[96];
    opt_budget_label(shown, label, sizeof label);
    f32 cw = opt_wide_w(), fh = ImGui::GetFrameHeight(), gap = dp(8), box_w = dp(92), save_w = dp(72);
    f32 slider_w = cw - box_w - save_w - fh - 3 * gap;
    opt_row_begin("Monthly budget", label, cw);
    f32 x = g_row.x1 - cw, y = opt_row_frame_y();
    f32 v = (f32)cbrt(fmin(fmax(shown, 0), R2_BUDGET_TOP) / R2_BUDGET_TOP);
    ImGui::SetCursorScreenPos(ImVec2(x, floorf(y + (fh - dp(24)) / 2)));
    if (lib_slider("##r2budget", &v, 0, 1, slider_w, dp(24))) {
        f64 d = R2_BUDGET_TOP * v * v * v;
        draft = (f32)(floor(d / 0.05 + 0.5) * 0.05);
    }
    // The exact amount, with the currency sign inside the box: what's typed is the draft as it's typed
    static char typed[24];
    static bool editing;
    if (!editing) snprintf(typed, sizeof typed, "%.2f", shown);
    const char *usd = opt_usd();
    f32 pad = dp(8), sign_w = text_w(g_font, usd);
    ImVec2 box(x + slider_w + gap, y);
    ImGui::SetCursorScreenPos(box);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad + sign_w + dp(3), ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(box_w);
    bool typed_now = ImGui::InputText("##r2budgettype", typed, sizeof typed,
                                      ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
    editing = ImGui::IsItemActive();
    bool left = ImGui::IsItemDeactivated();
    ImGui::PopStyleVar();
    opt_text_at(ImGui::GetWindowDrawList(), g_font, ImVec2(box.x + pad, floorf(box.y + (fh - g_font->FontSize) / 2)),
                COL_DIM, usd, box.x + box_w);
    if (typed_now || left) {
        char *end = 0;
        f64 d = strtod(typed, &end);
        if (end != typed && d >= 0) draft = (f32)(floor(fmin(d, R2_BUDGET_MOST) * 100 + 0.5) / 100);
    }
    ImGui::SetCursorScreenPos(ImVec2(box.x + box_w + gap, y));
    if (kit_button("Save##r2budgetsave", KIT_PRIMARY, ImVec2(save_w, 0), dirty)) {
        c.r2_budget = draft;
        draft = -1;
        log_line("options: R2 budget $%.2f a month", c.r2_budget);
        options_apply(0);
        r2_cleanup_now();
    }
    ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - fh, y));
    kit_next_tip("Put it back");
    if (kit_icon_button(ICON_UNDO, "##r2budgetundo", KIT_GHOST, dirty)) draft = -1;
    opt_row_end();
}

// The player's ShareX uploaders (.sxcu in the uploaders folder, r2.cpp): stem, name and the host they send to.
// Read again every 3 s, and at once after one is added or removed (link_uploader_add / _remove, library.cpp).
struct OptUploader {
    char stem[64], name[64], host[96];
};
static OptUploader g_uploaders[32];
static i32 g_uploader_count = -1;
static f64 g_uploaders_at;

static i32 opt_uploaders()
{
    f64 t = ImGui::GetTime();
    if (g_uploader_count >= 0 && t - g_uploaders_at < 3) return g_uploader_count;
    g_uploaders_at = t;
    static char stems[32][64];
    static Uploader u;
    i32 n = uploader_list(stems, COUNT(stems));
    for (i32 i = 0; i < n; i++) {
        OptUploader &o = g_uploaders[i];
        char why[160];
        str_set(o.stem, stems[i]);
        bool ok = uploader_load(stems[i], &u, why, sizeof why);
        str_set(o.name, ok && u.name[0] ? u.name : stems[i]);
        o.host[0] = 0;
        if (ok) {
            const char *h = strstr(u.url, "://");
            h = h ? h + 3 : u.url;
            snprintf(o.host, sizeof o.host, "%.*s", (int)strcspn(h, "/?:"), h);
        } else snprintf(o.host, sizeof o.host, "%s", why);
    }
    return g_uploader_count = n;
}

// Your servers: one row with an icon, the name, how many, a link to how to set one up, and Add. Once there's one,
// a list of OPT_UPLOADER_ROWS rows under it, paged so the card keeps its size: an icon, the name, the host, remove
// and a switch.
enum { OPT_UPLOADER_ROWS = 3 };

static void opt_uploader_rows()
{
    Config &c = g_app.config;
    static i32 page;
    static char note[200]; // what Add did, under the name
    i32 n = opt_uploaders(), pages = n ? (n + OPT_UPLOADER_ROWS - 1) / OPT_UPLOADER_ROWS : 1;
    if (page >= pages) page = pages - 1;
    f32 fh = ImGui::GetFrameHeight(), sw = dp(36), is = dp(30);
    {
        const char *how = "How to set one up";
        f32 bw = dp(96), arrows = 2 * fh + dp(4), how_w = text_w(g_font_small, how);
        opt_row_begin(0, 0, 0, dp(52));
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 ic(g_row.x0 + is / 2, g_row.y + g_row.h / 2);
        dl->AddCircleFilled(ic, is / 2, col32(COL_PANEL2));
        f32 gw = text_w(g_font_icon, ICON_GLOBE);
        opt_text_at(dl, g_font_icon, ImVec2(floorf(ic.x - gw / 2), floorf(ic.y - g_font_icon->FontSize / 2)), COL_DIM,
                    ICON_GLOBE, ic.x + is);
        f32 how_x = g_row.x1 - bw - dp(12) - arrows - dp(16) - how_w;
        f32 tx = g_row.x0 + is + dp(12), max_x = how_x - dp(20), nh = g_font->FontSize, sh = g_font_small->FontSize;
        f32 ty = note[0] ? floorf(g_row.y + (g_row.h - nh - dp(3) - sh) / 2) : opt_row_text_y(g_font);
        const char *name = "Your servers";
        opt_text_at(dl, g_font, ImVec2(tx, ty), COL_TEXT, name, max_x);
        if (n) {
            char count[16];
            snprintf(count, sizeof count, "%d", n);
            opt_pill(dl, ImVec2(tx + text_w(g_font, name) + dp(8), ty + nh / 2 + 1), count, COL_PANEL2, COL_DIM);
        }
        if (note[0]) opt_text_at(dl, g_font_small, ImVec2(tx, ty + nh + dp(3)), COL_DIM, note, max_x);
        // How to set one up: a small underlined link
        ImVec2 at(how_x, floorf(g_row.y + (g_row.h - sh) / 2));
        ImGui::SetCursorScreenPos(at);
        if (ImGui::InvisibleButton("##uphow", ImVec2(how_w, sh))) {
            dev_click("options", "servers how");
            open_url("https://getsharex.com/docs/custom-uploader");
        }
        bool hot = ImGui::IsItemHovered();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        opt_text_at(dl, g_font_small, at, hot ? COL_TEXT : COL_DIM, how, at.x + how_w + 1);
        dl->AddLine(ImVec2(at.x, at.y + sh), ImVec2(at.x + how_w, at.y + sh), col32(hot ? COL_TEXT : COL_DIM, 0.5f));
        f32 x = g_row.x1 - bw - dp(12) - arrows, y = opt_row_frame_y();
        if (pages > 1) {
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            kit_next_tip("Previous page");
            if (kit_icon_button(ICON_LEFT, "##uppageback", KIT_GHOST, page > 0)) page--;
            ImGui::SetCursorScreenPos(ImVec2(x + fh + dp(4), y));
            kit_next_tip("Next page");
            if (kit_icon_button(ICON_RIGHT, "##uppagenext", KIT_GHOST, page < pages - 1)) page++;
        }
        ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - bw, y));
        if (kit_button(ICON_ADD "  Add...##addup", KIT_SECONDARY, ImVec2(bw, 0))) {
            Path file = {};
            if (pick_file(file, &file, L"Servers", L"*.sxcu")) {
                char stem[64], why[160];
                if (link_uploader_add(file.s, stem, sizeof stem, why, sizeof why)) {
                    g_uploader_count = -1;
                    snprintf(note, sizeof note, "Added %s", stem);
                    // Show the page it's on
                    i32 m = opt_uploaders();
                    for (i32 i = 0; i < m; i++)
                        if (!strcmp(g_uploaders[i].stem, stem)) page = i / OPT_UPLOADER_ROWS;
                    options_apply(0);
                } else snprintf(note, sizeof note, "Couldn't add it: %s", why);
                log_line("options: server: %s", note);
            }
        }
        opt_row_end();
    }
    if (!n) return;
    for (i32 r = 0; r < OPT_UPLOADER_ROWS; r++) {
        i32 k = page * OPT_UPLOADER_ROWS + r;
        if (k >= n) {
            opt_row_begin(0, 0, 0, dp(52), false);
            opt_row_end();
            continue;
        }
        const OptUploader &u = g_uploaders[k];
        ImGui::PushID(k);
        opt_row_begin(0, 0, fh + dp(12) + sw, dp(52));
        ImDrawList *dl = ImGui::GetWindowDrawList();
        // The icon: the name's first letter on a disc
        ImVec2 ic(g_row.x0 + is / 2, g_row.y + g_row.h / 2);
        dl->AddCircleFilled(ic, is / 2, col32(COL_PANEL2));
        char letter[2] = {(char)toupper((u8)u.name[0]), 0};
        f32 lw = text_w(g_font_bold, letter);
        opt_text_at(dl, g_font_bold, ImVec2(floorf(ic.x - lw / 2), floorf(ic.y - g_font_bold->FontSize / 2)), COL_TEXT,
                    letter, ic.x + is);
        f32 tx = g_row.x0 + is + dp(12), max_x = g_row.x1 - fh - dp(12) - sw - dp(20);
        f32 nh = g_font->FontSize, sh = g_font_small->FontSize, ty = floorf(g_row.y + (g_row.h - nh - dp(3) - sh) / 2);
        opt_text_at(dl, g_font, ImVec2(tx, ty), COL_TEXT, u.name, max_x);
        opt_text_at(dl, g_font_small, ImVec2(tx, ty + nh + dp(3)), COL_DIM, u.host, max_x);
        f32 y = opt_row_frame_y();
        ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - sw - dp(12) - fh, y));
        kit_next_tip("Remove");
        if (kit_icon_button(ICON_DELETE, "##upremove", KIT_GHOST)) {
            char stem[64];
            str_set(stem, u.stem);
            if (!link_uploader_remove(stem)) snprintf(note, sizeof note, "Couldn't delete %s's file", stem);
            else note[0] = 0;
            g_uploader_count = -1;
            log_line("options: server %s removed", stem);
            options_apply(0);
        }
        ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - sw, y));
        bool on = link_host_on(u.stem);
        if (kit_toggle("##upon", &on)) {
            link_uploader_set(u.stem, on);
            log_line("options: servers \"%s\"", c.link_uploaders);
            options_apply(0);
        }
        ImGui::PopID();
        opt_row_end();
    }
}

// The reads limit last saved, so a drag saves once, when it ends
static i32 g_read_cap_saved = -1;

// The reads limit (r2_read_cap): bucket links pause near it until the month ends. A slider of 1 to 100 million in
// steps of a million with a tick at R2's free 10 million, and a box for the number. Over the free ones the line
// says what the month costs at the limit.
static void opt_read_cap_row()
{
    Config &c = g_app.config;
    static const i32 FREE_M = 10;
    static const f64 USD_PER_M = 0.36;
    i32 m = c.r2_read_cap / 1000000;
    m = m < 1 ? 1 : m > 100 ? 100 : m;
    char line[64];
    if (m <= FREE_M) str_set(line, "Free");
    else snprintf(line, sizeof line, "%s%.2f a month at the limit", opt_usd(), (m - FREE_M) * USD_PER_M);
    f32 cw = opt_wide_w(), fh = ImGui::GetFrameHeight(), gap = dp(8), box_w = dp(92);
    f32 slider_w = cw - box_w - gap;
    opt_row_begin("Reads limit", line, cw, dp(52));
    f32 x = g_row.x1 - cw, y = opt_row_frame_y();
    i32 was = m;
    f32 v = (f32)m;
    ImGui::SetCursorScreenPos(ImVec2(x, floorf(y + (fh - dp(24)) / 2)));
    if (lib_slider("##readcap", &v, 1, 100, slider_w, dp(24), (f32)FREE_M)) m = (i32)(v + 0.5f);
    bool slid = ImGui::IsItemDeactivated();
    // The number, in millions, with the M inside the box
    static char typed[16];
    static bool editing;
    if (!editing) snprintf(typed, sizeof typed, "%d", m);
    f32 pad = dp(8), unit_w = text_w(g_font, "M");
    ImVec2 box(x + slider_w + gap, y);
    ImGui::SetCursorScreenPos(box);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad, ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(box_w);
    ImGui::InputText("##readcaptype", typed, sizeof typed, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
    editing = ImGui::IsItemActive();
    bool typed_done = ImGui::IsItemDeactivated();
    ImGui::PopStyleVar();
    opt_text_at(ImGui::GetWindowDrawList(), g_font, ImVec2(box.x + box_w - pad - unit_w, floorf(box.y + (fh - g_font->FontSize) / 2)),
                COL_DIM, "M", box.x + box_w);
    if (typed_done) {
        i32 t = atoi(typed);
        if (t >= 1) m = t > 100 ? 100 : t;
    }
    if (m != was) c.r2_read_cap = m * 1000000;
    if ((slid || typed_done) && c.r2_read_cap != g_read_cap_saved) {
        g_read_cap_saved = c.r2_read_cap;
        log_line("options: R2 reads limit %dM", m);
        options_apply(0);
    }
    opt_row_end();
}

// A small underlined link in the dim small font at `at`; true when clicked.
static bool opt_link_at(const char *id, ImVec2 at, const char *text)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 w = text_w(g_font_small, text), sh = g_font_small->FontSize;
    ImGui::SetCursorScreenPos(at);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, sh));
    bool hot = ImGui::IsItemHovered();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    opt_text_at(dl, g_font_small, at, hot ? COL_TEXT : COL_DIM, text, at.x + w + 1);
    dl->AddLine(ImVec2(at.x, at.y + sh), ImVec2(at.x + w, at.y + sh), col32(hot ? COL_TEXT : COL_DIM, 0.5f));
    return clicked;
}

// A host row's name: the host's own icon, the name (bold when `bold`) and an info icon after it that opens the
// host's page; under them `sub` when given. Takes the row's left part up to max_x.
static void opt_host_name(LinkHost host, const char *name, bool bold, const char *url, const char *sub, u32 sub_col,
                          bool dot, f32 max_x)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 lg = dp(22), x = g_row.x0;
    host_logo_draw(dl, host, ImVec2(x, floorf(g_row.y + (g_row.h - lg) / 2)), lg);
    f32 x0 = g_row.x0;
    g_row.x0 = x + lg + dp(10);
    ImFont *nf = bold ? g_font_bold : g_font;
    if (sub) opt_row_lines(name, nf, sub, sub_col, dot, max_x);
    else opt_text_at(dl, nf, ImVec2(g_row.x0, opt_row_text_y(nf)), COL_TEXT, name, max_x);
    f32 nh = nf->FontSize, sh = g_font_small->FontSize;
    f32 ty = sub ? floorf(g_row.y + (g_row.h - nh - dp(3) - sh) / 2) : opt_row_text_y(nf);
    f32 iz = g_font_small->FontSize + dp(4);
    ImVec2 at(g_row.x0 + text_w(nf, name) + dp(6), floorf(ty + (nh - iz) / 2));
    g_row.x0 = x0;
    ImGui::SetCursorScreenPos(at);
    char id[48];
    snprintf(id, sizeof id, "##hostinfo%s", name);
    if (ImGui::InvisibleButton(id, ImVec2(iz, iz))) {
        dev_click("options", name, "info");
        open_url(url);
    }
    bool hot = ImGui::IsItemHovered();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    kit_tip("Their page, in the browser");
    dl->AddText(g_font_icon, iz, at, col32(hot ? COL_TEXT : COL_DIM), ICON_INFO);
}

// A bitrate in kbit/s as Mbps for a box: "8", "1.5", "0.25"
static void opt_mbps_text(i32 kbps, char *out, i32 cap)
{
    if (kbps % 1000 == 0) snprintf(out, cap, "%d", kbps / 1000);
    else if (kbps % 100 == 0) snprintf(out, cap, "%.1f", kbps / 1000.0);
    else snprintf(out, cap, "%.2f", kbps / 1000.0);
}

// Quality: "Use recommended", with the source of its numbers (SHARE_KBPS_DEFAULT) as a link under it, and the
// switch. Off (share_kbps_custom), the table opens under it with the player's own numbers, kept while the switch is
// on: the bitrate for each picture height at 30 and 60 fps, a box per cell in Mbps. Reset, in its slot beside the
// switch while the table's open, puts the recommended numbers in the table; dimmed while nothing differs.
static void opt_quality_table()
{
    Config &c = g_app.config;
    static char typed[6][2][16];
    static i32 editing = -1; // the cell being typed in, row * 2 + column
    f32 cw = opt_wide_w(), fh = ImGui::GetFrameHeight(), sh = g_font_small->FontSize, nh = g_font->FontSize;
    f32 reset_w = dp(72), toggle_w = kit_px(36), gap = dp(8);
    opt_row_begin(0, 0, reset_w + gap + toggle_w, dp(52));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 ty = floorf(g_row.y + (g_row.h - nh - dp(3) - sh) / 2);
    opt_text_at(dl, g_font, ImVec2(g_row.x0, ty), COL_TEXT, "Use recommended", g_row.x1 - reset_w - gap - toggle_w - dp(20));
    if (opt_link_at("##kbpssource", ImVec2(g_row.x0, ty + nh + dp(3)), "Source: YouTube recommended upload settings")) {
        dev_click("options", "bitrate source");
        open_url("https://support.google.com/youtube/answer/1722171");
    }
    f32 fy = opt_row_frame_y();
    if (c.share_kbps_custom) {
        ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - toggle_w - gap - reset_w, fy));
        bool reset = kit_button("Reset##kbpsreset", KIT_SECONDARY, ImVec2(reset_w, 0), !share_kbps_is_default());
        kit_tip("Put the recommended numbers in the table");
        if (reset) {
            share_kbps_reset();
            editing = -1;
            log_line("options: share bitrates reset");
            options_apply(0);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - toggle_w, fy));
    bool reco = !c.share_kbps_custom;
    if (kit_toggle("##kbpsreco", &reco)) {
        c.share_kbps_custom = !reco;
        editing = -1;
        log_line("options: share bitrates %s", reco ? "recommended" : "custom");
        options_apply(0);
    }
    opt_row_end();
    if (!c.share_kbps_custom) return;

    f32 head_h = dp(22), cell_h = fh + dp(6), pad = dp(4);
    opt_row_begin(0, 0, cw, head_h + 6 * cell_h + 2 * pad, false);
    f32 x = g_row.x1 - cw, label_w = dp(64), box_w = floorf((cw - label_w - gap) / 2);
    f32 y = g_row.y + pad;
    static const char *const heads[2] = {"30 fps", "60 fps"};
    for (i32 k = 0; k < 2; k++)
        opt_text_at(dl, g_font_small, ImVec2(x + label_w + k * (box_w + gap), y + floorf((head_h - sh) / 2)), COL_DIM,
                    heads[k], x + label_w + k * (box_w + gap) + box_w);
    y += head_h;
    f32 in_pad = dp(8), unit_w = text_w(g_font, "Mbps");
    for (i32 r = 0; r < 6; r++, y += cell_h) {
        char label[16];
        snprintf(label, sizeof label, "%dp", SHARE_KBPS_HEIGHTS[r]);
        opt_text_at(dl, g_font, ImVec2(x, y + floorf((fh - g_font->FontSize) / 2)), COL_DIM, label, x + label_w);
        for (i32 k = 0; k < 2; k++) {
            i32 cell = r * 2 + k;
            if (editing != cell) opt_mbps_text(c.share_kbps[r][k], typed[r][k], sizeof typed[r][k]);
            ImVec2 box(x + label_w + k * (box_w + gap), y);
            char id[24];
            snprintf(id, sizeof id, "##kbps%d", cell);
            ImGui::SetCursorScreenPos(box);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(in_pad, ImGui::GetStyle().FramePadding.y));
            ImGui::SetNextItemWidth(box_w);
            ImGui::InputText(id, typed[r][k], sizeof typed[r][k], ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
            if (ImGui::IsItemActive()) editing = cell;
            else if (editing == cell) editing = -1;
            bool done = ImGui::IsItemDeactivated();
            ImGui::PopStyleVar();
            opt_text_at(dl, g_font, ImVec2(box.x + box_w - in_pad - unit_w, floorf(box.y + (fh - g_font->FontSize) / 2)),
                        COL_DIM, "Mbps", box.x + box_w);
            if (done) {
                f64 v = atof(typed[r][k]);
                i32 kbps = (i32)(v * 1000 + 0.5);
                if (v > 0 && kbps != c.share_kbps[r][k]) {
                    share_kbps_set(r, k == 1, kbps);
                    log_line("options: share bitrate %dp %s %d kbit/s", SHARE_KBPS_HEIGHTS[r], k ? "60" : "30",
                             c.share_kbps[r][k]);
                    options_apply(0);
                }
            }
        }
    }
    opt_row_end();
}

// The Lower quality to fit setting last saved, so a drag saves once, when it ends
static i32 g_min_quality_saved = -1;

// Lower quality to fit (share_min_quality): how far under the recommended bitrate a file may go to fit the max
// size, from 100% (never) to 50% in steps of 1%, with a tick at 67% and a box for the number. The line is an
// example from the Quality table.
static void opt_min_quality_row()
{
    Config &c = g_app.config;
    i32 q = c.share_min_quality < 50 ? 50 : c.share_min_quality > 100 ? 100 : c.share_min_quality;
    char line[64];
    if (q >= 100) str_set(line, "Bigger clips go as a link");
    else snprintf(line, sizeof line, "1080p 60 can drop to %.1f Mbps", share_lowest_kbps(1920, 1080, 60, q) / 1000);
    f32 cw = opt_wide_w(), fh = ImGui::GetFrameHeight(), gap = dp(8), box_w = dp(72);
    f32 slider_w = cw - box_w - gap;
    opt_row_begin("Lower quality to fit", line, cw, dp(52));
    f32 x = g_row.x1 - cw, y = opt_row_frame_y();
    i32 was = q;
    f32 v = (f32)q;
    ImGui::SetCursorScreenPos(ImVec2(x, floorf(y + (fh - dp(24)) / 2)));
    if (lib_slider("##minquality", &v, 50, 100, slider_w, dp(24), 67)) q = (i32)(v + 0.5f);
    bool slid = ImGui::IsItemDeactivated();
    static char typed[8];
    static bool editing;
    if (!editing) snprintf(typed, sizeof typed, "%d", q);
    f32 pad = dp(8), unit_w = text_w(g_font, "%");
    ImVec2 box(x + slider_w + gap, y);
    ImGui::SetCursorScreenPos(box);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad, ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(box_w);
    ImGui::InputText("##minqualitytype", typed, sizeof typed, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
    editing = ImGui::IsItemActive();
    bool typed_done = ImGui::IsItemDeactivated();
    ImGui::PopStyleVar();
    opt_text_at(ImGui::GetWindowDrawList(), g_font, ImVec2(box.x + box_w - pad - unit_w, floorf(box.y + (fh - g_font->FontSize) / 2)),
                COL_DIM, "%", box.x + box_w);
    if (typed_done && typed[0]) {
        i32 t = atoi(typed);
        q = t < 50 ? 50 : t > 100 ? 100 : t;
    }
    if (q != was) c.share_min_quality = q;
    if ((slid || typed_done) && c.share_min_quality != g_min_quality_saved) {
        g_min_quality_saved = c.share_min_quality;
        log_line("options: share min quality %d%%", c.share_min_quality);
        options_apply(0);
    }
    opt_row_end();
}

// The clip a link card's preview shows: the newest clip with a game, else the newest clip, else none.
static const LibItem *opt_card_sample()
{
    Library &lib = g_app.library;
    const LibItem *best = 0;
    for (i32 i = 0; i < lib.item_count; i++) {
        const LibItem &it = lib.items[i];
        if (it.kind != LIB_CLIPS || !it.probed) continue;
        bool better = !best || (it.info.game[0] && !best->info.game[0]) ||
                      (!it.info.game[0] == !best->info.game[0] && it.when_us > best->when_us);
        if (better) best = &it;
    }
    return best;
}

// The card styles as chips, two rows across the control column; shown only when there's more than one. Returns
// the style under the mouse, for the preview, else -1.
static i32 opt_card_styles()
{
    Config &c = g_app.config;
    i32 n = embed_style_count();
    if (n < 2) return -1;
    i32 per = (n + 1) / 2, hot = -1;
    f32 cw = opt_wide_w(), gap = dp(6), fh = ImGui::GetFrameHeight(), chip_w = floorf((cw - (per - 1) * gap) / per);
    opt_row_begin("Style", "What the card shows", cw, 2 * fh + gap + dp(24));
    f32 x = g_row.x1 - cw, y = g_row.y + dp(12);
    for (i32 i = 0; i < n; i++) {
        ImGui::SetCursorScreenPos(ImVec2(x + (i % per) * (chip_w + gap), y + (i / per) * (fh + gap)));
        char label[64];
        snprintf(label, sizeof label, "%s##cardstyle%d", embed_style_name(i), i);
        if (kit_chip(label, c.embed_style == i, 0, chip_w)) {
            c.embed_style = (u8)i;
            log_line("options: card style %s", embed_style_id(i));
            options_apply(0);
        }
        if (ImGui::IsItemHovered()) hot = i;
    }
    opt_row_end();
    return hot;
}

// Our own icon, for the card's "via" line: the exe's, as the window's.
static ID3D11ShaderResourceView *opt_own_icon()
{
    static ID3D11ShaderResourceView *srv;
    static bool tried;
    if (!tried) {
        tried = true;
        Path exe = {};
        GetModuleFileNameW(0, exe.s, MAX_PATH);
        srv = app_icon_load(exe);
    }
    return srv;
}

// A link's card as Discord shows it in `style` (-1: the player's), built by embed_card as the page's is: the title
// in bold, the line ("Game - Name") with the game's small icon before it, the video with a play button (or the
// picture), and the small grey "via" line with our icon. The row's height is fixed at the tallest card, so nothing below moves.
static void opt_link_card_preview(i32 style)
{
    const LibItem *it = opt_card_sample();
    char title[200];
    str_set(title, !it ? "Clip" : it->display_name[0] ? it->display_name : it->name);
    const char *game = it ? it->info.game : "";
    EmbedInput in = {title, game, it ? it->info.duration : 42, 0, now_us(), true, g_app.config.accent};
    EmbedCard card;
    embed_card(in, &card, style);

    char line[400];
    str_set(line, card.line);
    if (card.tag[0]) snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s", line[0] ? " - " : "", card.tag);

    f32 cw = opt_wide_w(), pad = dp(12), bar = dp(4), lgap = dp(6);
    f32 title_h = g_font_bold->FontSize, line_h = g_font->FontSize, via_h = g_font_small->FontSize;
    f32 vid_w = cw - bar - 2 * pad, vid_h = floorf(vid_w * 9 / 16);
    f32 tall = pad + title_h + lgap + line_h + dp(10) + vid_h + dp(8) + via_h + pad;
    f32 head_h = line[0] ? title_h + lgap + line_h : title_h;
    f32 card_h = pad + head_h + dp(10) + vid_h + (card.via ? dp(8) + via_h : 0) + pad;
    opt_row_begin("Preview", "How Discord shows a link from your bucket", cw, tall + dp(24));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a(g_row.x1 - cw, g_row.y + dp(12)), b(g_row.x1, g_row.y + dp(12) + card_h);
    // Discord's dark theme, whatever ours is
    const u32 BG = 0x2b2d31, EDGE = 0x1e1f22, TITLE = 0xf2f3f5, LINE = 0xb5bac1, FAINT = 0x949ba4;
    dl->AddRectFilled(a, b, col32(BG), dp(8));
    dl->AddRect(a, b, col32(EDGE), dp(8));
    if (card.accent) dl->AddRectFilled(a, ImVec2(a.x + bar, b.y), col32(card.accent), dp(8), ImDrawFlags_RoundCornersLeft);
    f32 x = a.x + bar + pad, x1 = b.x - pad, y = a.y + pad;
    opt_text_at(dl, g_font_bold, ImVec2(x, y), TITLE, card.title, x1);
    if (line[0]) {
        f32 ly = y + title_h + lgap, lx = x;
        // The game's emoji, as Discord shows it: only for a game that has one
        char emoji[96];
        bool has = card.icon && game[0] && gamedb_emoji(game, emoji, sizeof emoji);
        ID3D11ShaderResourceView *icon = has ? game_icon(game) : 0;
        if (icon) { // the game's icon before the line, the line's height
            f32 is = floorf(line_h);
            dl->AddImageRounded((ImTextureID)(intptr_t)icon, ImVec2(lx, ly), ImVec2(lx + is, ly + is), ImVec2(0, 0),
                                ImVec2(1, 1), IM_COL32_WHITE, dp(3));
            lx += is + dp(6);
        }
        opt_text_at(dl, g_font, ImVec2(lx, ly), LINE, line, x1);
    }
    y += head_h + dp(10);
    ImVec2 v0(x, y), v1(x + vid_w, y + vid_h);
    LibThumb *th = it ? lib_thumb(*it) : 0;
    if (th && th->srv)
        dl->AddImageRounded((ImTextureID)(intptr_t)th->srv, v0, v1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, dp(6));
    else dl->AddRectFilled(v0, v1, col32(0x111214), dp(6));
    if (card.player) { // the play button over the picture
        ImVec2 m((v0.x + v1.x) / 2, (v0.y + v1.y) / 2);
        f32 r = dp(22);
        dl->AddCircleFilled(m, r, IM_COL32(0, 0, 0, 150), 32);
        f32 t = r * 0.42f;
        dl->AddTriangleFilled(ImVec2(m.x - t * 0.7f, m.y - t), ImVec2(m.x - t * 0.7f, m.y + t), ImVec2(m.x + t, m.y),
                              IM_COL32_WHITE);
    }
    y += vid_h + dp(8);
    if (card.via) { // our icon, then "via"
        f32 vx = x;
        if (ID3D11ShaderResourceView *own = opt_own_icon()) {
            f32 is = floorf(via_h);
            dl->AddImage((ImTextureID)(intptr_t)own, ImVec2(vx, y), ImVec2(vx + is, y + is));
            vx += is + dp(4);
        }
        opt_text_at(dl, g_font_small, ImVec2(vx, y), FAINT, "via " APP_NAME, x1);
    }
    if (th && th->pending) ui_wake();
    opt_row_end();
}

static void options_sharing()
{
    Config &c = g_app.config;
    f32 fw = dp(360), bw = dp(150);
    {
        ImVec2 top = ImGui::GetCursorScreenPos();
        f32 full = ImGui::GetContentRegionAvail().x, sz = g_font_small->FontSize + dp(4);
        opt_section("SHARING");
        ImVec2 back = ImGui::GetCursorScreenPos(), at(top.x + full - sz - dp(4), top.y - dp(2));
        ImGui::SetCursorScreenPos(at);
        if (ImGui::InvisibleButton("##sharinghelp", ImVec2(sz, sz))) {
            dev_click("options", "sharing help");
            g_help_jump = "SHARING";
            g_app.ui.help_open = true;
        }
        bool hot = ImGui::IsItemHovered();
        kit_tip("How sharing works, in Help");
        ImGui::GetWindowDrawList()->AddText(g_font_icon, sz, at, col32(hot ? COL_TEXT : COL_DIM), ICON_INFO);
        ImGui::SetCursorScreenPos(back);
    }
    opt_card_begin();
    // The largest file Share sends: five sizes and Custom as chips across the control column. Picking Custom turns
    // that chip into its number box, the same size, so nothing moves.
    {
        static const f32 sizes[] = {20, 50, 100, 500, 2048};
        static bool custom;
        i32 n = (i32)COUNT(sizes), at = -1;
        for (i32 i = 0; i < n; i++)
            if (fabsf(c.share_max_mb - sizes[i]) < 0.5f) at = i;
        if (at < 0) custom = true;
        f32 cw = opt_wide_w(), gap = dp(6), chip_w = floorf((cw - n * gap) / (n + 1));
        opt_row_begin("Max file size", 0, cw);
        f32 x = g_row.x1 - cw, y = opt_row_frame_y();
        for (i32 i = 0; i <= n; i++) {
            ImGui::SetCursorScreenPos(ImVec2(x + i * (chip_w + gap), y));
            if (i == n && custom) {
                ImGui::SetNextItemWidth(chip_w);
                if (ImGui::InputFloat("##sharemax", &c.share_max_mb, 0, 0, "%.0f MB")) {
                    c.share_max_mb = c.share_max_mb < 1 ? 1 : c.share_max_mb > 100000 ? 100000 : c.share_max_mb;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    log_line("options: share max %g MB", c.share_max_mb);
                    options_apply(0);
                }
                continue;
            }
            char label[32];
            if (i < n) {
                share_size_label(sizes[i], label, sizeof label);
                snprintf(label + strlen(label), sizeof label - strlen(label), "##max%d", i);
            } else snprintf(label, sizeof label, "Custom##maxc");
            if (kit_chip(label, i < n && !custom && at == i, 0, chip_w)) {
                custom = i == n;
                if (i < n) c.share_max_mb = sizes[i];
                log_line("options: share max %s", custom ? "custom" : label);
                options_apply(0);
            }
        }
        opt_row_end();
    }

    opt_quality_table();
    opt_min_quality_row();

    // Catbox, on or off
    {
        opt_row_begin(0, 0, dp(36));
        opt_host_name(HOST_CATBOX, "Catbox", false, "https://catbox.moe/faq.php", 0, 0, false, g_row.x1 - dp(56));
        ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - dp(36), opt_row_frame_y()));
        if (kit_toggle("##catbox", &c.link_catbox)) {
            log_line("options: Catbox %s", c.link_catbox ? "on" : "off");
            options_apply(0);
        }
        char tip[160];
        r2_rates_tip(c.upload_rate, c.upload_rate_n, r2_set_up(), tip, sizeof tip);
        kit_tip(tip);
        opt_row_end();
    }

    opt_uploader_rows();

    // Cloudflare: the player's own R2 bucket. One row for the account: who and what the setup is doing under the
    // name, then Set up again and one button that signs in, waits (a click gives up), or signs out.
    R2Status st = r2_status();
    bool signed_in = r2_signed_in(), has = r2_has_token(), waiting = st.state == R2_SETUP_WAITING;
    {
        f32 fh = ImGui::GetFrameHeight(), mw = text_w(g_font, "Sign in again") + dp(32), gap = dp(8);
        opt_row_begin(0, 0, mw + gap + fh, dp(60));
        char text[400];
        if (st.state == R2_SETUP_IDLE) str_set(text, r2_ready() ? "Set up" : "Not set up");
        else if (st.who[0] && st.state == R2_SETUP_OK) snprintf(text, sizeof text, "%s  -  %s", st.who, st.text);
        else str_set(text, st.text);
        u32 col = st.state == R2_SETUP_OK ? COL_OK
                  : st.state == R2_SETUP_FAILED || st.state == R2_SETUP_NO_TOKEN_SCOPE ? COL_WARN : COL_DIM;
        opt_host_name(HOST_R2, "Cloudflare", true, "https://developers.cloudflare.com/r2/pricing/", text, col, col != COL_DIM,
                      g_row.x1 - mw - gap - fh - dp(20));
        f32 x = g_row.x1 - mw - gap - fh, y = opt_row_frame_y();
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        kit_next_tip("Set up again");
        if (kit_icon_button(ICON_REFRESH, "##r2again", KIT_GHOST,
                            (signed_in || has) && st.state != R2_SETUP_BUSY && st.state != R2_SETUP_NO_TOKEN_SCOPE &&
                                !waiting)) {
            log_line("options: R2 set up again");
            r2_setup_again();
        }
        ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - mw, y));
        if (waiting) {
            kit_next_tip("Stop waiting");
            if (kit_button("Waiting...##r2main", KIT_SECONDARY, ImVec2(mw, 0))) {
                dev_click("options", "r2 sign-in cancel");
                r2_sign_in_cancel();
            }
        } else if (st.state == R2_SETUP_NO_TOKEN_SCOPE || (!signed_in && !has)) {
            if (kit_button(signed_in || has || st.state == R2_SETUP_NO_TOKEN_SCOPE ? "Sign in again##r2main" : "Sign in##r2main",
                           KIT_PRIMARY, ImVec2(mw, 0))) {
                dev_click("options", "r2 sign in");
                r2_sign_in();
            }
        } else {
            kit_next_tip("Forgets the keys. The bucket and its links stay");
            if (kit_button("Sign out##r2main", KIT_SECONDARY, ImVec2(mw, 0))) {
                log_line("options: R2 sign out");
                r2_sign_out();
            }
        }
        if (st.state == R2_SETUP_BUSY || waiting) ui_wake();
        opt_row_end();
    }

    opt_budget_row();
    opt_space_row(r2_limit_for_budget(g_budget_draft >= 0 ? g_budget_draft : c.r2_budget), g_budget_draft >= 0);
    opt_reads_row();

    // Last in the card, so opening them moves nothing above: the Advanced rows, and the token rows for a token made
    // by hand. Each link has a slot as wide as its longer wording, so neither moves when the other is clicked.
    static bool advanced, token_rows;
    opt_row_begin(0, 0, 0, dp(36));
    {
        struct Fold {
            const char *open, *shut, *id;
            bool *on;
        } folds[] = {{"Advanced", "Hide advanced", "##r2advanced", &advanced},
                     {"Use a token instead", "Hide the token rows", "##r2tokenrows", &token_rows}};
        f32 x = g_row.x0;
        for (Fold &f : folds) {
            const char *link = *f.on ? f.shut : f.open;
            f32 lw = text_w(g_font_small, link);
            ImVec2 at(x, floorf(g_row.y + (g_row.h - g_font_small->FontSize) / 2));
            ImGui::SetCursorScreenPos(at);
            if (ImGui::InvisibleButton(f.id, ImVec2(lw, g_font_small->FontSize))) *f.on = !*f.on;
            bool hot = ImGui::IsItemHovered();
            if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            opt_text_at(dl, g_font_small, at, hot ? COL_TEXT : COL_DIM, link, g_row.x1);
            dl->AddLine(ImVec2(at.x, at.y + g_font_small->FontSize), ImVec2(at.x + lw, at.y + g_font_small->FontSize),
                        col32(hot ? COL_TEXT : COL_DIM, 0.5f));
            x += fmaxf(text_w(g_font_small, f.open), text_w(g_font_small, f.shut)) + dp(24);
        }
    }
    opt_row_end();
    // Public address: the bucket's r2.dev address or a custom domain connected to it in Cloudflare
    if (advanced) {
        R2Domains d = r2_domains(r2_ready());
        if (d.state == -1 || !r2_set_up()) {
            // Cloudflare wouldn't list the domains (or there's no bucket yet): the address is typed
            char why[200];
            if (r2_set_up()) snprintf(why, sizeof why, "Couldn't list the bucket's domains: %s", d.why);
            opt_row_begin("Public address", r2_set_up() ? why : 0, fw, dp(52));
            char t[MAX_PATH * 3];
            if (opt_text("##r2public", c.r2_public_url, fw, 0, "https://", t, sizeof t)) {
                char *v = trim(t);
                if (strcmp(v, c.r2_public_url)) {
                    str_set(c.r2_public_url, v);
                    log_line("options: R2 public address %s", v[0] ? "set" : "cleared");
                    options_apply(0);
                    if (!v[0] && (signed_in || has)) r2_setup_again();
                }
            }
            opt_row_end();
        } else {
            // r2.dev first, then the custom domains, then the address in use when it's neither
            char names[10][136];
            const char *items[10], *notes[10] = {};
            i32 n = 0, cur = -1;
            snprintf(names[n], sizeof names[n], "%s", d.r2dev[0] ? d.r2dev : "The bucket's r2.dev address");
            notes[n] = "r2.dev";
            n++;
            for (i32 i = 0; i < d.count; i++) snprintf(names[n++], sizeof names[0], "%s", d.list[i]);
            for (i32 i = 0; i < n; i++) {
                items[i] = names[i];
                char url[160];
                snprintf(url, sizeof url, "https://%s", names[i]);
                if (!strcmp(url, c.r2_public_url)) cur = i;
            }
            if (cur < 0 && strstr(c.r2_public_url, ".r2.dev")) cur = 0;
            if (cur < 0 && c.r2_public_url[0]) {
                const char *u = c.r2_public_url;
                if (starts_with(u, "https://")) u += 8;
                snprintf(names[n], sizeof names[n], "%s", u);
                items[n] = names[n];
                cur = n++;
            }
            opt_row_begin("Public address", d.state == 1 && !d.count && !d.r2dev[0] ? "Asking Cloudflare..." : 0, fw, dp(52));
            if (opt_combo("##r2public", items, notes, n, &cur, fw)) {
                if (cur == 0 && !(d.r2dev[0] && d.r2dev_on)) {
                    c.r2_public_url[0] = 0; // setting up turns the r2.dev address on
                    r2_setup_again();
                } else snprintf(c.r2_public_url, sizeof c.r2_public_url, "https://%s", items[cur]);
                log_line("options: R2 public address %s", c.r2_public_url[0] ? c.r2_public_url : "r2.dev");
                options_apply(0);
            }
            opt_row_end();
        }
        opt_read_cap_row();
    }
    if (token_rows) {
        opt_row_begin("Create token", 0, bw);
        if (kit_button("Create token##r2token", KIT_SECONDARY, ImVec2(bw, 0))) {
            dev_click("options", "r2 create token");
            open_url(r2_token_link());
        }
        opt_row_end();

        // The token: masked, kept by Windows; clipback makes the bucket and its keys from it
        {
            f32 field_w = fw - ImGui::GetFrameHeight() - dp(8);
            opt_row_begin("API token", 0, fw);
            char s[512];
            if (opt_text("##r2tok", "", field_w, ImGuiInputTextFlags_Password, has ? "Saved" : "Paste the token", s, sizeof s) &&
                trim(s)[0]) {
                r2_set_token(trim(s));
                log_line("options: R2 token saved, setting up");
            }
            if (ImGui::IsItemDeactivated()) SecureZeroMemory(g_app.options.edit, sizeof g_app.options.edit);
            SecureZeroMemory(s, sizeof s);
            ImGui::SameLine(0, dp(8));
            kit_next_tip("Forget the token");
            if (kit_icon_button(ICON_DELETE, "##r2forget", KIT_GHOST, has)) {
                r2_set_token("");
                log_line("options: R2 token removed");
            }
            opt_row_end();
        }
    }
    opt_card_end();

    // A link's card in Discord: how it looks
    opt_section("LINK CARD");
    opt_card_begin();
    // The small copy a link's card plays: its size as chips across the control column
    {
        static const f32 sizes[] = {50, 80, 100, 150, 200};
        i32 n = (i32)COUNT(sizes);
        f32 cw = opt_wide_w(), gap = dp(6), chip_w = floorf((cw - (n - 1) * gap) / n);
        opt_row_begin("Card video size", "The small copy the card plays. Too big and it won't play", cw);
        f32 x = g_row.x1 - cw, y = opt_row_frame_y();
        for (i32 i = 0; i < n; i++) {
            ImGui::SetCursorScreenPos(ImVec2(x + i * (chip_w + gap), y));
            char label[32];
            snprintf(label, sizeof label, "%.0f MB##social%d", sizes[i], i);
            if (kit_chip(label, fabsf(c.social_copy_mb - sizes[i]) < 0.5f, 0, chip_w)) {
                c.social_copy_mb = sizes[i];
                log_line("options: card video %g MB", c.social_copy_mb);
                options_apply(0);
            }
        }
        opt_row_end();
    }
    opt_link_card_preview(opt_card_styles()); // a hovered chip's style shows in the preview
    opt_card_end();
}

static void options_integrations()
{
    Config &c = g_app.config;
    static char sd_msg[160];
    static bool sd_msg_error;
    // Looking at processes and files costs a little: once a second is plenty for a status
    static f64 checked;
    static bool sd_have, sd_running, plugin_in, plugin_with_us;
    f64 now = ImGui::GetTime();
    if (now - checked > 1 || !checked) {
        checked = now;
        Path p;
        sd_have = streamdeck_exe(&p);
        sd_running = process_find(L"StreamDeck.exe") != 0;
        Path dest = streamdeck_plugin_dest();
        wchar_t manifest[MAX_PATH];
        swprintf(manifest, MAX_PATH, L"%s\\manifest.json", dest.s);
        plugin_in = dest.s[0] && path_is_file(manifest);
        plugin_with_us = streamdeck_plugin_source(&p);
    }
    i32 keys = pipe_clients("streamdeck"), others = pipe_clients(0) - keys;
    f32 fh = ImGui::GetFrameHeight(), bw = dp(150), sw = dp(300);

    opt_section("STREAM DECK");
    opt_card_begin();
    opt_row_begin("Stream Deck", "Elgato's app for its keypads", sw + dp(12) + bw);
    {
        f32 x = ImGui::GetCursorScreenPos().x;
        opt_status(!sd_have ? "Not installed" : sd_running ? "Running" : "Installed, not running",
                   !sd_have ? COL_DIM : sd_running ? COL_OK : COL_WARN, x + sw, x);
        ImGui::SetCursorScreenPos(ImVec2(x + sw + dp(12), opt_row_frame_y()));
        if (!sd_have) {
            if (kit_button("Get Stream Deck##getsd", KIT_SECONDARY, ImVec2(bw, 0))) open_url("https://www.elgato.com/downloads");
        } else if (kit_button(sd_running ? "Show##opensd" : "Start##opensd", KIT_SECONDARY, ImVec2(bw, 0))) {
            Path sd;
            if (streamdeck_exe(&sd)) ShellExecuteW(0, L"open", sd.s, 0, 0, SW_SHOWNORMAL);
        }
    }
    opt_row_end();

    opt_row_begin(APP_NAME " keys", "Save clip, Recording on / off and Open " APP_NAME, sw + dp(12) + bw);
    {
        f32 x = ImGui::GetCursorScreenPos().x;
        opt_status(plugin_in ? "Installed" : "Not installed", plugin_in ? COL_OK : COL_DIM, x + sw, x);
        ImGui::SetCursorScreenPos(ImVec2(x + sw + dp(12), opt_row_frame_y()));
        if (kit_button(plugin_in ? "Reinstall##sdinstall" : "Install##sdinstall", plugin_in ? KIT_SECONDARY : KIT_PRIMARY,
                       ImVec2(bw, 0), sd_have && plugin_with_us)) {
            sd_msg_error = !streamdeck_install(sd_msg, sizeof sd_msg);
            checked = 0;
        }
    }
    opt_row_end();

    char conn[96];
    if (keys) snprintf(conn, sizeof conn, "Connected");
    else snprintf(conn, sizeof conn, "%s", !plugin_in ? "Install the keys first" : !sd_running ? "Stream Deck isn't running" : "Waiting for Stream Deck");
    opt_row_begin("Connection", sd_msg[0] ? sd_msg : "The keys talk to " APP_NAME " on this PC only", sw + dp(12) + bw);
    {
        f32 x = ImGui::GetCursorScreenPos().x;
        opt_status(conn, keys ? COL_OK : sd_msg_error ? COL_WARN : COL_DIM, x + sw, x);
    }
    opt_row_end();
    opt_card_end();

    opt_section("VIDEO EDITORS");
    opt_card_begin();
    static const char *ed_names[2] = {"Premiere Pro", "DaVinci Resolve"};
    for (i32 e = 0; e < 2; e++) {
        bool premiere = e == 0;
        Path exe = {};
        bool have = editor_exe(premiere, &exe);
        Path &set = premiere ? c.premiere : c.resolve;
        char shown[MAX_PATH * 3] = "Not found";
        if (have) wide_to_utf8(exe.s, shown, sizeof shown);
        ImGui::PushID(e);
        opt_row_begin(ed_names[e], have ? shown : "Not found: pick its exe to open clips in it", bw * 2 + dp(8));
        if (kit_button(have ? "Change##pick" : "Choose its exe##pick", KIT_SECONDARY, ImVec2(bw, 0))) {
            Path picked = {};
            if (pick_file(exe, &picked, L"Programs", L"*.exe")) {
                set = picked;
                char u[MAX_PATH * 3];
                wide_to_utf8(picked.s, u, sizeof u);
                log_line("options: %s is %s", ed_names[e], u);
                options_apply(0);
            }
        }
        ImGui::SameLine(0, dp(8));
        ImGui::BeginDisabled(!set.s[0]);
        if (kit_button("Find it again##auto", KIT_SECONDARY, ImVec2(bw, 0))) {
            set.s[0] = 0;
            log_line("options: %s found by itself", ed_names[e]);
            options_apply(0);
        }
        ImGui::EndDisabled();
        opt_row_end();
        ImGui::PopID();
    }
    opt_card_end();

    options_sharing();

    if (others > 0) {
        opt_section("OTHER PROGRAMS");
        opt_card_begin();
        char n[64];
        snprintf(n, sizeof n, "%d connected", others);
        opt_row_value("On the pipe", "Programs driving " APP_NAME ", such as lifelog's voice commands", n, COL_TEXT);
        opt_card_end();
    }
    (void)fh;
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
    kit_next_tip("Choose a folder");
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

// The accent row's height and its colour wheel's size, at 100%.
static const f32 OPT_ACCENT_H = 184, OPT_WHEEL = 164;

// The accent colour: a hue wheel with a saturation and brightness triangle inside, and left of it the colour in a
// box as hex (typed to set it exactly) and a button back to the default, which stays put and is only greyed while
// the default is in use. The app takes the colour as it changes; it is saved when the mouse lets go.
static void opt_accent_row()
{
    Config &c = g_app.config;
    f32 fh = ImGui::GetFrameHeight(), d = dp(OPT_WHEEL), box_w = dp(112), gap = dp(8);
    f32 bars = fh, inner = ImGui::GetStyle().ItemInnerSpacing.x;
    opt_row_begin("Accent colour", 0, d + gap + fh + gap + box_w, dp(OPT_ACCENT_H));
    static bool unsaved;
    bool changed = false;

    // The wheel: ImGui's picker with nothing but the wheel and triangle; its centre sits bars/2 right of its x
    f32 wy = g_row.y + floorf((g_row.h - d) / 2);
    ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - d - bars / 2, wy));
    ImGui::SetNextItemWidth(d + bars + inner);
    f32 col[3] = {((c.accent >> 16) & 255) / 255.f, ((c.accent >> 8) & 255) / 255.f, (c.accent & 255) / 255.f};
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    if (ImGui::ColorPicker3("##accentwheel", col,
                            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview |
                                ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_NoInputs |
                                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoTooltip |
                                ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_NoDragDrop |
                                ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_DisplayRGB)) {
        u32 v = 0;
        for (i32 i = 0; i < 3; i++) v = v << 8 | (u32)(fminf(fmaxf(col[i], 0), 1) * 255 + 0.5f);
        if (v != c.accent) {
            c.accent = v;
            changed = true;
        }
    }
    ImGui::PopStyleVar();

    // The box: a dot of the colour, then its hex; what's typed applies once it is six digits
    f32 y = opt_row_frame_y(), bx = g_row.x1 - d - gap - fh - gap - box_w, dot = dp(7);
    static char typed[8];
    static bool editing;
    if (!editing) snprintf(typed, sizeof typed, "%06x", c.accent);
    ImGui::SetCursorScreenPos(ImVec2(bx, y));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(dp(10) + 2 * dot + dp(8), ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(box_w);
    bool typed_now = ImGui::InputText("##accenthex", typed, sizeof typed,
                                      ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_AutoSelectAll);
    editing = ImGui::IsItemActive(); // left half-typed, it shows the colour in use again
    ImGui::PopStyleVar();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 dc(bx + dp(10) + dot, y + fh / 2);
    dl->AddCircleFilled(dc, dot, col32(c.accent));
    dl->AddCircle(dc, dot, col32(0xffffff, 0.15f), 0, dp(1));
    if (typed_now && strlen(typed) == 6) {
        u32 v = (u32)strtoul(typed, 0, 16);
        if (v != c.accent) {
            c.accent = v;
            changed = true;
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(bx + box_w + gap, y));
    kit_next_tip("Default colour");
    if (kit_icon_button(ICON_UNDO, "##accentreset", KIT_GHOST, c.accent != ACCENT_DEFAULT)) {
        c.accent = ACCENT_DEFAULT;
        changed = true;
    }
    opt_row_end();

    if (changed) {
        ui_apply_accent(c.accent);
        unsaved = true;
    }
    if (unsaved && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !editing) {
        unsaved = false;
        log_line("options: accent %06x", c.accent);
        options_apply(0);
    }
}

// The monitor map's height, fixed so nothing moves whatever the monitors are
static const f32 OPT_MAP_H = 132;

// The corner slot c of monitor rect a..b in the map: a quarter of its width by a sixth of its height, inset
static void opt_map_corner(ImVec2 a, ImVec2 b, i32 c, ImVec2 *s0, ImVec2 *s1)
{
    f32 in = dp(5), w = floorf((b.x - a.x) * 0.26f), h = floorf((b.y - a.y) * 0.16f);
    bool right = c == CORNER_BR || c == CORNER_TR, top = c == CORNER_TR || c == CORNER_TL;
    f32 x = right ? b.x - in - w : a.x + in, y = top ? a.y + in : b.y - in - h;
    *s0 = ImVec2(x, y);
    *s1 = ImVec2(x + w, y + h);
}

// A dashed outline round a..b
static void opt_dashed_rect(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col)
{
    f32 dash = dp(3), gap = dp(3), t = dp(1);
    for (f32 x = a.x; x < b.x; x += dash + gap) {
        f32 x1 = fminf(x + dash, b.x);
        dl->AddRectFilled(ImVec2(x, a.y), ImVec2(x1, a.y + t), col);
        dl->AddRectFilled(ImVec2(x, b.y - t), ImVec2(x1, b.y), col);
    }
    for (f32 y = a.y; y < b.y; y += dash + gap) {
        f32 y1 = fminf(y + dash, b.y);
        dl->AddRectFilled(ImVec2(a.x, y), ImVec2(a.x + t, y1), col);
        dl->AddRectFilled(ImVec2(b.x - t, y), ImVec2(b.x, y1), col);
    }
}

static const char *const CORNER_NAMES[] = {"Bottom right", "Bottom left", "Top right", "Top left"};

// The monitors as Windows lays them out, to scale, cw x OPT_MAP_H from the cursor. `shown` is the monitor picked
// (an output index), outlined in the accent with its four corner slots, `corner` filled in as a small toast. The
// monitor recorded has a red dot. A click on a monitor picks it, on a corner slot (of any monitor) it and the
// corner: *mon_out / *corner_out, true. Greyed and still while `live` is false.
static bool opt_monitor_map(const char *id, i32 shown, PopupCorner corner, bool live, i32 *mon_out, PopupCorner *corner_out)
{
    OptionsUi &o = g_app.options;
    f32 cw = opt_wide_w(), h = dp(OPT_MAP_H);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    if (!live) ImGui::BeginDisabled();
    bool clicked = ImGui::InvisibleButton("##map", ImVec2(cw, h));
    if (!live) ImGui::EndDisabled();
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    i32 n = o.monitor_count;
    if (n <= 0) {
        opt_text_at(dl, g_font, ImVec2(a.x, floorf(a.y + (h - g_font->FontSize) / 2)), COL_DIM, "No monitor found", a.x + cw);
        return false;
    }
    // The desktop's bounds, scaled into the box and centred
    RECT all = g_monitor_rect[0];
    for (i32 i = 1; i < n; i++) {
        const RECT &r = g_monitor_rect[i];
        all.left = r.left < all.left ? r.left : all.left;
        all.top = r.top < all.top ? r.top : all.top;
        all.right = r.right > all.right ? r.right : all.right;
        all.bottom = r.bottom > all.bottom ? r.bottom : all.bottom;
    }
    f32 dw = (f32)(all.right - all.left), dh = (f32)(all.bottom - all.top);
    f32 sc = fminf(cw / fmaxf(dw, 1), h / fmaxf(dh, 1));
    ImVec2 o0(floorf(a.x + (cw - dw * sc) / 2), floorf(a.y + (h - dh * sc) / 2));
    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool over = live && ImGui::IsItemHovered();
    f32 alpha = live ? 1 : 0.4f;
    i32 rec = g_app.config.output_idx;
    bool picked = false;
    for (i32 i = 0; i < n; i++) {
        const RECT &r = g_monitor_rect[i];
        f32 gap = dp(2);
        ImVec2 m0(floorf(o0.x + (r.left - all.left) * sc) + gap, floorf(o0.y + (r.top - all.top) * sc) + gap);
        ImVec2 m1(floorf(o0.x + (r.right - all.left) * sc) - gap, floorf(o0.y + (r.bottom - all.top) * sc) - gap);
        bool on = i == shown, hot = over && mouse.x >= m0.x && mouse.x < m1.x && mouse.y >= m0.y && mouse.y < m1.y;
        f32 rr = dp(6);
        dl->AddRectFilled(m0, m1, col32(on || hot ? COL_HOVER : COL_PANEL2, alpha), rr);
        dl->AddRect(m0, m1, col32(on ? COL_ACCENT : COL_LINE, alpha), rr, 0, on ? dp(2) : dp(1));
        // Its number, centred, with the recorded one's dot after it
        char num[8];
        snprintf(num, sizeof num, "%d", i + 1);
        ImVec2 ts = g_font_bold->CalcTextSizeA(g_font_bold->FontSize, FLT_MAX, 0, num);
        ImVec2 tc(floorf((m0.x + m1.x - ts.x) / 2), floorf((m0.y + m1.y - ts.y) / 2));
        dl->AddText(g_font_bold, g_font_bold->FontSize, tc, col32(on ? COL_TEXT : COL_DIM, alpha), num);
        if (i == rec) dl->AddCircleFilled(ImVec2(tc.x + ts.x + dp(7), tc.y + ts.y / 2), dp(3), col32(COL_RED, alpha));
        // Corner slots: the picked monitor's always, the one under the mouse's too
        i32 hot_corner = -1;
        if (on || hot)
            for (i32 c = 0; c < 4; c++) {
                ImVec2 s0, s1;
                opt_map_corner(m0, m1, c, &s0, &s1);
                bool chot = hot && mouse.x >= s0.x && mouse.x < s1.x && mouse.y >= s0.y && mouse.y < s1.y;
                if (chot) hot_corner = c;
                if (on && c == corner) {
                    dl->AddRectFilled(s0, s1, col32(COL_PANEL, alpha), dp(3));
                    dl->AddRect(s0, s1, col32(COL_ACCENT, alpha), dp(3), 0, dp(1));
                } else {
                    opt_dashed_rect(dl, s0, s1, col32(chot ? COL_TEXT : COL_DIM, alpha * (on ? 1 : 0.6f)));
                }
            }
        if (live) {
            char tip[160];
            if (hot_corner >= 0) {
                ImVec2 s0, s1;
                opt_map_corner(m0, m1, hot_corner, &s0, &s1);
                kit_tip_rect(s0, s1, CORNER_NAMES[hot_corner]);
            } else {
                char name[96], note[64];
                monitor_label(i, name, sizeof name);
                monitor_note(i, note, sizeof note);
                snprintf(tip, sizeof tip, "%s, %s%s", name, note, i == rec ? ", recorded" : "");
                kit_tip_rect(m0, m1, tip);
            }
        }
        if (clicked && hot && !picked) {
            picked = true;
            *mon_out = i;
            *corner_out = hot_corner >= 0 ? (PopupCorner)hot_corner : corner;
        }
    }
    return picked;
}

// The map at a glance, h high from `a` and at most max_w wide, against the right of that: the monitors as outlines,
// the one picked in the accent with a dot in its corner. Returns the width it took.
static f32 opt_mini_map(ImVec2 a, f32 h, f32 max_w, i32 shown, PopupCorner corner)
{
    i32 n = g_app.options.monitor_count;
    if (n <= 0) return 0;
    RECT all = g_monitor_rect[0];
    for (i32 i = 1; i < n; i++) {
        const RECT &r = g_monitor_rect[i];
        all.left = r.left < all.left ? r.left : all.left;
        all.top = r.top < all.top ? r.top : all.top;
        all.right = r.right > all.right ? r.right : all.right;
        all.bottom = r.bottom > all.bottom ? r.bottom : all.bottom;
    }
    f32 dw = (f32)(all.right - all.left), dh = (f32)(all.bottom - all.top);
    f32 sc = fminf(max_w / fmaxf(dw, 1), h / fmaxf(dh, 1)), w = floorf(dw * sc);
    ImVec2 o0(a.x + max_w - w, floorf(a.y + (h - dh * sc) / 2));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (i32 i = 0; i < n; i++) {
        const RECT &r = g_monitor_rect[i];
        f32 gap = dp(1);
        ImVec2 m0(floorf(o0.x + (r.left - all.left) * sc) + gap, floorf(o0.y + (r.top - all.top) * sc) + gap);
        ImVec2 m1(floorf(o0.x + (r.right - all.left) * sc) - gap, floorf(o0.y + (r.bottom - all.top) * sc) - gap);
        bool on = i == shown;
        dl->AddRectFilled(m0, m1, col32(on ? COL_HOVER : COL_PANEL2), dp(2));
        dl->AddRect(m0, m1, col32(on ? COL_ACCENT : COL_LINE), dp(2), 0, dp(1));
        if (on) {
            f32 in = dp(4);
            bool right = corner == CORNER_BR || corner == CORNER_TR, top = corner == CORNER_TR || corner == CORNER_TL;
            dl->AddCircleFilled(ImVec2(right ? m1.x - in : m0.x + in, top ? m0.y + in : m1.y - in), dp(2), col32(COL_ACCENT));
        }
    }
    return w;
}

// The output index a popup monitor setting shows on the map: POPUP_RECORDED is the monitor recorded; one no longer
// connected shows as the first.
static i32 opt_map_index(i32 v)
{
    i32 n = g_app.options.monitor_count;
    if (v == POPUP_RECORDED) v = g_app.config.output_idx;
    return v >= 0 && v < n ? v : 0;
}

// A pick on the map as a setting: the monitor recorded stays POPUP_RECORDED, so it follows Recording's monitor
static i32 opt_map_value(i32 i) { return i == g_app.config.output_idx ? POPUP_RECORDED : i; }

// Popups (Config.popup_monitor, popup_corner): the map alone. Other place in games: a switch, off as
// POPUP_AS_NORMAL; on, a map of its own (popup_monitor_game, popup_corner_game) under it, starting from Popups'.
// Both behind "Popup areas": closed (as it starts), the row has the normal choice as a mini map and Edit; open, the
// two rows follow it.
static void opt_popup_maps()
{
    Config &c = g_app.config;
    static i32 listed_frame = -100;
    static bool open;
    if (ImGui::GetFrameCount() > listed_frame + 1) options_list_monitors(); // entered again: the monitors may have changed
    listed_frame = ImGui::GetFrameCount();
    f32 cw = opt_wide_w(), mh = dp(OPT_MAP_H), pad = dp(12);

    f32 edit_w = dp(84), mini_w = dp(64), mini_h = dp(20);
    opt_row_begin("Popup areas", 0, mini_w + dp(12) + edit_w);
    f32 fy = opt_row_frame_y();
    ImVec2 ma(g_row.x1 - edit_w - dp(12) - mini_w, floorf(g_row.y + (g_row.h - mini_h) / 2));
    opt_mini_map(ma, mini_h, mini_w, opt_map_index(c.popup_monitor), c.popup_corner);
    ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - edit_w, fy));
    bool flip = kit_button(open ? "Edit  " ICON_CHEVRON_UP "##popupareas" : "Edit  " ICON_DOWN "##popupareas", KIT_SECONDARY,
                           ImVec2(edit_w, 0));
    kit_tip(open ? "Hide where the popups go" : "Where the popups go: a monitor and a corner");
    if (flip) {
        open = !open;
        dev_click("options", "popup areas", open ? "open" : "shut");
    }
    opt_row_end();
    if (!open) return;

    opt_row_begin("Popups", 0, cw, mh + 2 * pad);
    ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - cw, g_row.y + pad));
    i32 mon;
    PopupCorner corner;
    if (opt_monitor_map("popups", opt_map_index(c.popup_monitor), c.popup_corner, true, &mon, &corner)) {
        c.popup_monitor = opt_map_value(mon);
        c.popup_corner = corner;
        log_line("options: popups on %d, %s", c.popup_monitor, corner_name(corner));
        options_apply(0);
    }
    opt_row_end();

    bool other = c.popup_monitor_game != POPUP_AS_NORMAL;
    if (opt_row_toggle("Other place in games", 0, "##popupsgameon", &other)) {
        c.popup_monitor_game = other ? c.popup_monitor : POPUP_AS_NORMAL;
        c.popup_corner_game = c.popup_corner;
        log_line("options: popups in games %d", c.popup_monitor_game);
        options_apply(0);
    }
    if (!other) return;
    opt_row_begin(0, 0, cw, mh + 2 * pad);
    ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - cw, g_row.y + pad));
    if (opt_monitor_map("popupsgame", opt_map_index(c.popup_monitor_game), c.popup_corner_game, true, &mon, &corner)) {
        c.popup_monitor_game = opt_map_value(mon);
        c.popup_corner_game = corner;
        log_line("options: popups in games on %d, %s", c.popup_monitor_game, corner_name(corner));
        options_apply(0);
    }
    opt_row_end();
}

// Playable copies of edits turned off, not saved yet: the copies show as going until Save turns render_edits off
// or the undo button drops it.
static bool g_render_edits_off;

// Playable copies of edits (render_edits): a bar of the clips' space with the rendered copies at its end and their
// size beside it, then Save, undo and the switch, in fixed slots. Turning it off is a draft that shows the copies
// going; turning it on applies at once.
static void opt_copies_row()
{
    Config &c = g_app.config;
    const Library &lib = g_app.library;
    bool &off = g_render_edits_off;
    if (!c.render_edits) off = false;
    i64 copies_bytes = 0, clips_bytes = 0;
    i32 copies = render_copies(&copies_bytes);
    for (i32 i = 0; i < lib.item_count; i++) clips_bytes += (i64)lib.items[i].size;
    char size[32], sub[96];
    bytes_text(copies_bytes, size, sizeof size);
    u32 sub_col = COL_DIM;
    if (off && copies) {
        snprintf(sub, sizeof sub, "Save frees %d cop%s, %s", copies, copies == 1 ? "y" : "ies", size);
        sub_col = COL_RED;
    } else if (off || !c.render_edits) str_set(sub, "Edits play only in the library");
    else str_set(sub, "Edited clips in Clips play as edited anywhere");
    f32 cw = opt_wide_w(), fh = ImGui::GetFrameHeight(), gap = dp(8), save_w = dp(72), toggle_w = kit_px(36);
    f32 text_w_ = dp(72), bar_w = cw - text_w_ - save_w - fh - toggle_w - 4 * gap, bh = dp(OPT_BAR_H);
    opt_row_begin(0, 0, cw, dp(52));
    opt_row_lines("Playable copies of edits", g_font, sub, sub_col, false, g_row.x1 - cw - dp(20));
    f32 x = g_row.x1 - cw, y = opt_row_frame_y();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a(x, floorf(g_row.y + (g_row.h - bh) / 2)), z(x + bar_w, a.y + bh);
    dl->AddRectFilled(a, z, col32(COL_PANEL2), dp(3));
    i64 total = clips_bytes + copies_bytes;
    if (total > 0) {
        f32 cx = floorf(a.x + bar_w * (f32)((f64)clips_bytes / total));
        dl->PushClipRect(a, ImVec2(cx, z.y), true);
        dl->AddRectFilled(a, z, col32(COL_BLUE, 0.8f), dp(3));
        dl->PopClipRect();
        if (copies_bytes > 0) {
            dl->PushClipRect(ImVec2(fminf(cx, z.x - dp(3)), a.y), z, true);
            dl->AddRectFilled(a, z, col32(off ? COL_RED : COL_OK), dp(3));
            dl->PopClipRect();
        }
    }
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##copiesbar", ImVec2(bar_w, bh));
    char clips_text[32], tip[96];
    bytes_text(clips_bytes, clips_text, sizeof clips_text);
    snprintf(tip, sizeof tip, "Clips %s, playable copies %s", clips_text, size);
    kit_tip(tip);
    opt_text_at(dl, g_font_small, ImVec2(z.x + dp(10), floorf(a.y + (bh - g_font_small->FontSize) / 2)),
                off && copies ? COL_RED : COL_DIM, copies ? size : "None", z.x + dp(10) + text_w_);
    f32 bx = z.x + gap + text_w_ + gap;
    ImGui::SetCursorScreenPos(ImVec2(bx, y));
    if (kit_button("Save##copiessave", KIT_PRIMARY, ImVec2(save_w, 0), off)) {
        c.render_edits = false;
        off = false;
        log_line("options: playable copies of edits off, %d copies go", copies);
        options_apply(0);
    }
    ImGui::SetCursorScreenPos(ImVec2(bx + save_w + gap, y));
    kit_next_tip("Put it back");
    if (kit_icon_button(ICON_UNDO, "##copiesundo", KIT_GHOST, off)) off = false;
    ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - toggle_w, y));
    bool v = c.render_edits && !off;
    if (kit_toggle("##renderedits", &v)) {
        if (!v) off = true;
        else if (off) off = false;
        else {
            c.render_edits = true;
            log_line("options: playable copies of edits on");
            options_apply(0);
        }
    }
    opt_row_end();
}

static void options_general()
{
    Config &c = g_app.config;

    opt_section("LOOK");
    opt_card_begin();
    opt_accent_row();
    static const char *const clocks[] = {"12-hour", "24-hour"}, *const clock_notes[] = {"9:04 pm", "21:04"};
    i32 clock = c.clock_24h ? 1 : 0;
    if (opt_row_combo("Clock", 0, "##clock", clocks, clock_notes, 2, &clock, dp(200))) {
        c.clock_24h = clock == 1;
        log_line("options: %s clock", c.clock_24h ? "24-hour" : "12-hour");
        options_apply(0);
    }
    if (opt_row_toggle("Tooltips", "Point at a button to see what it does and its key", "##tooltips", &c.tooltips)) {
        log_line("options: tooltips %s", c.tooltips ? "on" : "off");
        options_apply(0);
    }
    if (opt_row_toggle("Flat look", "Plain flat colours: no game art behind the window, no grain", "##flatlook",
                       &c.flat_look)) {
        log_line("options: flat look %s", c.flat_look ? "on" : "off");
        options_apply(0);
    }
    // Where the toasts go: Clip saved, Session ended and the marker window, a monitor and a corner on a map of them.
    // In games picks another while a game is in front. Clip saved popup picks how Clip saved shows over a game:
    // whole, the small pill or not at all.
    opt_popup_maps();
    opt_row_begin("Clip saved popup", "Over a game. A game can have its own, in Apps", toast_clip_chips_w(TOAST_CLIP_FULL));
    if (toast_clip_chips("clip", TOAST_CLIP_FULL, &c.toast_clip)) {
        log_line("options: clip saved popup: %s", toast_clip_name(c.toast_clip));
        options_apply(0);
    }
    opt_row_end();
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
    {
        // What saves are named: the template, and under it what a save of the game in front would be called now
        f32 field_w = fmaxf(dp(220), fminf(dp(420), (g_card.w - 2 * dp(OPT_PAD)) * 0.55f));
        char example[256], sub[320];
        save_file_name(g_app.audio.game.s, "", now_us(), example, sizeof example);
        snprintf(sub, sizeof sub, "{game} {YYYY} {YY} {MM} {DD} {hh} {mm} {ss}, as in %s", example);
        opt_row_begin("File names", sub, field_w);
        char text[sizeof c.name_template];
        if (opt_text("##names", c.name_template, field_w, 0, SAVE_NAME_DEFAULT, text, sizeof text)) {
            char *t = trim(text);
            if (!*t) t = (char *)SAVE_NAME_DEFAULT;
            if (strcmp(t, c.name_template)) {
                str_set(c.name_template, t);
                log_line("options: file names %s", c.name_template);
                options_apply(0);
                library_rescan(); // names are read with the template
            }
        }
        opt_row_end();
    }
    if (opt_row_folder("Buffer folder", 0, "buffer", &c.buffer_dir)) options_apply(APPLY_RECORDER);
    if (opt_row_folder("Playback cache", "Where the Timeline builds what it plays; a fast drive starts sooner", "play",
                       &c.play_dir))
        options_apply(0);
    if (opt_row_f32("Keep share copies for", "Share's small copies go this long after they were last shared. Clips stay", "##share", &c.share_keep_days, 0.1, 365, "days")) {
        log_line("options: share copies kept %g days", c.share_keep_days);
        options_apply(0);
    }
    opt_copies_row();
    opt_card_end();

    // The editor's waveforms, cached so a file opens with them at once. Counted when the section comes into view
    // and after a clear, not every frame.
    opt_section("AUDIO PREVIEWS");
    static i32 wave_files, wave_read_frame;
    static u64 wave_bytes;
    i32 wave_frame = ImGui::GetFrameCount();
    if (!wave_read_frame || wave_frame - wave_read_frame > 1) wave_cache_stats(&wave_files, &wave_bytes);
    wave_read_frame = wave_frame;
    opt_card_begin();
    {
        char sub[96], size[24];
        fmt_size(wave_bytes, size, sizeof size);
        if (wave_files) snprintf(sub, sizeof sub, "Waveforms of %d file%s, %s", wave_files, wave_files == 1 ? "" : "s", size);
        else str_set(sub, "None cached");
        f32 cw = text_w(g_font, "Clear") + dp(32), fh = ImGui::GetFrameHeight();
        opt_row_begin("Cached waveforms", sub, fh + dp(8) + cw);
        kit_next_tip("Open the folder");
        if (kit_icon_button(ICON_FOLDER, "##wavedir", KIT_SECONDARY)) opt_open_dir(path_join(g_app.paths.local_dir, L"thumbs"));
        ImGui::SameLine(0, dp(8));
        if (kit_button("Clear##wavecache", KIT_SECONDARY, ImVec2(cw, 0), wave_files > 0)) {
            i32 gone = wave_cache_clear(0);
            log_line("options: cleared %d audio previews", gone);
            wave_cache_stats(&wave_files, &wave_bytes);
        }
        opt_row_end();
    }
    if (opt_row_f32("Keep for", "Ones not opened in this long are deleted; 0 keeps them", "##wavekeep",
                    &c.wave_keep_days, 0, 3650, "days")) {
        log_line("options: audio previews kept %g days", c.wave_keep_days);
        options_apply(0);
    }
    opt_card_end();

    opt_section("PRIVACY");
    opt_card_begin();
    if (opt_row_toggle("Look up games online", "Names, icons and art from Discord, Steam, SteamGridDB and IGDB. Applies from the next start",
                       "##lookup", &c.game_lookup)) {
        log_line("options: look up games %s", c.game_lookup ? "on" : "off");
        options_apply(0);
    }
    ImGui::BeginDisabled(!c.game_lookup);
    if (opt_row_toggle("Send usage stats", "Once a day: the version, Windows build, encoder, and how many clips, saves, shares and links. No names or files",
                       "##stats", &c.send_stats)) {
        log_line("options: usage stats %s", c.send_stats ? "on" : "off");
        options_apply(0);
    }
    ImGui::EndDisabled();
    opt_card_end();

    opt_section("GAMES");
    opt_card_begin();
    ImGui::BeginDisabled(!c.game_lookup);
    bool service = c.art_service.s[0] != 0;
    if (opt_row_toggle("Art from " APP_NAME "'s service", "Art Discord and Steam don't have, from SteamGridDB and IGDB. Sends game names and store ids only",
                       "##artservice", &service)) {
        str_set(c.art_service.s, service ? ART_SERVICE_HOST : "");
        log_line("options: art service %s", service ? "on" : "off");
        options_apply(0);
    }
    ImGui::BeginDisabled(!service);
    if (opt_row_toggle("Help name games", "When you name a game " APP_NAME " couldn't place, that name helps name it for other players",
                       "##sharenames", &c.share_names)) {
        log_line("options: help name games %s", c.share_names ? "on" : "off");
        options_apply(0);
    }
    f32 code_w = text_w(g_font, "View the code") + dp(32);
    opt_row_begin("The service is open source", "Read exactly what it does with a request", code_w);
    if (kit_button("View the code##repo", KIT_SECONDARY, ImVec2(code_w, 0))) open_url(OPT_API_REPO_URL);
    opt_row_end();
    ImGui::EndDisabled();
    f32 key_w = fminf(dp(300), ImGui::GetContentRegionAvail().x * 0.45f);
    opt_row_begin("Your own SteamGridDB key", "Optional, for art the service doesn't hand out yet. Stays in settings.txt", key_w);
    char key[sizeof c.sgdb_key];
    if (opt_text("##sgdbkey", c.sgdb_key, key_w, ImGuiInputTextFlags_Password, "Paste it here", key, sizeof key)) {
        str_set(c.sgdb_key, trim(key));
        log_line("options: own SteamGridDB key %s", c.sgdb_key[0] ? "set" : "cleared");
        options_apply(0);
    }
    opt_row_end();
    f32 sgdb_w = text_w(g_font, "Open SteamGridDB") + dp(32);
    opt_row_begin("Get a key", "Sign in at steamgriddb.com, open Preferences > API, then Generate API Key", sgdb_w);
    if (kit_button("Open SteamGridDB##sgdb", KIT_SECONDARY, ImVec2(sgdb_w, 0))) open_url(OPT_SGDB_KEY_URL);
    opt_row_end();
    ImGui::EndDisabled();
    opt_row_begin("Cached game art", "Names, icons and covers fetched so far", ImGui::GetFrameHeight());
    kit_next_tip("Open the folder");
    if (kit_icon_button(ICON_FOLDER, "##artdir", KIT_SECONDARY)) opt_open_dir(path_join(g_app.paths.local_dir, L"gamedb"));
    opt_row_end();
    opt_card_end();

    // Backup: a copy of sessions (and clips) on another drive or a NAS (backup.cpp). The status line moves while it runs.
    opt_section("BACKUP");
    opt_card_begin();
    if (opt_row_toggle("Back up sessions", "Copies new ones every 30 minutes. Nothing at the backup is ever deleted",
                       "##backup", &c.backup_on)) {
        log_line("options: backup %s", c.backup_on ? "on" : "off");
        options_apply(0);
        if (c.backup_on) backup_now();
    }
    if (opt_row_folder("Backup folder", "Another drive, or a NAS folder such as \\\\nas\\clips", "backupdir", &c.backup_dir)) {
        options_apply(0);
        if (c.backup_on) backup_now();
    }
    if (opt_row_toggle("Clips too", "Not only sessions", "##backupclips", &c.backup_clips)) {
        log_line("options: backup clips %s", c.backup_clips ? "on" : "off");
        options_apply(0);
    }
    {
        char status[300];
        backup_status(status, sizeof status);
        f32 now_w = text_w(g_font, "Back up now") + dp(32);
        opt_row_begin("Status", status, now_w);
        if (kit_button("Back up now##backupnow", KIT_SECONDARY, ImVec2(now_w, 0),
                       c.backup_on && c.backup_dir.s[0] && g_backup.state != BACKUP_RUNNING))
            backup_now();
        opt_row_end();
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
    const char *runs = !set ? "Nothing runs at sign-in now" : !_wcsicmp(now_value, cmd) ? "This " APP_NAME " runs at sign-in" : u;
    if (is_test_copy()) runs = "Test copy: the Run list is left alone";
    opt_card_begin();
    // On is this exe in the Run list (the installer puts it there); nothing else keeps the setting.
    bool on = set && !_wcsicmp(now_value, cmd);
    ImGui::BeginDisabled(is_test_copy());
    if (opt_row_toggle("Start with Windows", runs, "##startup", &on)) {
        startup_set(on);
        read_frame = 0; // read it again
    }
    ImGui::EndDisabled();
    f32 sw = text_w(g_font, "Run again") + dp(32);
    opt_row_begin("First-run setup", "The clip key, tooltips and helping name games", sw);
    if (kit_button("Run again##setup", KIT_SECONDARY, ImVec2(sw, 0))) {
        g_app.ui.setup_open = true;
        log_line("options: setup run again");
    }
    opt_row_end();
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

    // Feedback: a kind, the text and Send; the answer shows under the box, where room is kept for it.
    opt_section("FEEDBACK");
    opt_card_begin();
    {
        static i32 kind = 0;
        static char text[FB_TEXT_MAX + 1];
        bool busy = g_tel.fb_busy != 0;
        f32 chip_w = dp(64), send_w = text_w(g_font, "Sending") + dp(32), gap = dp(8);
        opt_row_begin("A bug, an idea or anything else", c.game_lookup ? "Goes to " APP_NAME "'s service with the version number" : "Needs Look up games online",
                      3 * chip_w + 2 * gap + dp(16) + send_w);
        static const char *const labels[] = {"Bug##fbkind", "Idea##fbkind", "Other##fbkind"};
        for (i32 i = 0; i < 3; i++) {
            if (i) ImGui::SameLine(0, gap);
            if (kit_chip(labels[i], kind == i, 0, chip_w)) kind = i;
        }
        ImGui::SameLine(0, dp(16));
        bool any = false;
        for (const char *p = text; *p && !any; p++) any = !isspace((u8)*p);
        f32 wait = busy ? 0 : feedback_wait_left(g_tel.fb_sent_ms, GetTickCount64());
        bool can = c.game_lookup && !busy && wait <= 0 && any;
        if (kit_button(busy ? "Sending##fbsend" : wait > 0 ? "##fbsend" : "Send##fbsend", KIT_PRIMARY,
                       ImVec2(send_w, 0), can)) {
            dev_click("feedback", FEEDBACK_KINDS[kind]);
            feedback_send(kind, text);
        }
        if (wait > 0) { // the wait after a send: a ring that empties, in the greyed button
            ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
            ImVec2 mid((a.x + z.x) / 2, (a.y + z.y) / 2);
            f32 r = (z.y - a.y) * 0.28f, top = -IM_PI / 2;
            ImDrawList *rl = ImGui::GetWindowDrawList();
            rl->AddCircle(mid, r, col32(COL_FAINT, 0.5f), 32, dp(2));
            rl->PathArcTo(mid, r, top, top + 2 * IM_PI * wait, 32);
            rl->PathStroke(col32(COL_DIM), 0, dp(2));
            ui_wake(2); // it moves on its own
        }
        opt_row_end();
        f32 x0 = g_card.a.x + dp(OPT_PAD), box_w = g_card.w - 2 * dp(OPT_PAD);
        ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + dp(4)));
        ImGui::BeginDisabled(!c.game_lookup || busy);
        ImGui::InputTextMultiline("##fbtext", text, sizeof text, ImVec2(box_w, dp(96)));
        ImGui::EndDisabled();
        static bool cleared;
        if (!busy && g_tel.fb_ok && !cleared) { // sent: the box empties once
            text[0] = 0;
            cleared = true;
        }
        if (busy) cleared = false;
        f32 ly = ImGui::GetCursorScreenPos().y + dp(4);
        char count[48];
        snprintf(count, sizeof count, "%d of %d", (int)strlen(text), FB_TEXT_MAX);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (!busy && g_tel.fb_result[0])
            opt_text_at(dl, g_font_small, ImVec2(x0, ly), g_tel.fb_ok ? COL_OK : 0xff8a92, g_tel.fb_result, x0 + box_w - dp(100));
        f32 cw = text_w(g_font_small, count);
        opt_text_at(dl, g_font_small, ImVec2(x0 + box_w - cw, ly), COL_DIM, count, x0 + box_w + 1);
        ImGui::SetCursorScreenPos(ImVec2(g_card.a.x, ly + g_font_small->FontSize + dp(10)));
    }
    opt_card_end();

    opt_section("ABOUT");
    opt_card_begin();
    f32 gh_w = text_w(g_font, "GitHub") + dp(32), sup_w = OPT_SUPPORT_URL[0] ? text_w(g_font, "Support") + dp(32) : 0;
    f32 bgap = ImGui::GetStyle().ItemSpacing.x;
    opt_row_begin(APP_NAME " " CLIPBACK_VERSION, "Built " __DATE__, gh_w + (sup_w > 0 ? bgap + sup_w : 0));
    if (sup_w > 0) { // a quiet Support beside GitHub
        if (kit_button("Support##support", KIT_GHOST, ImVec2(sup_w, 0))) open_url(OPT_SUPPORT_URL);
        ImGui::SameLine(0, bgap);
    }
    if (kit_button("GitHub##apprepo", KIT_SECONDARY, ImVec2(gh_w, 0))) open_url(OPT_APP_REPO_URL);
    opt_row_end();
    if (opt_row_toggle("Update by itself", "Fetches new versions and checks they're signed. You choose when to restart",
                       "##autoupdate", &c.auto_update)) {
        log_line("options: update by itself %s", c.auto_update ? "on" : "off");
        if (c.auto_update) update_start();
        else update_stop();
        options_apply(0);
    }
    // The licence button only for the build clipback fetched (ffget.cpp); one found on the PATH has its own.
    Path licence = path_join(ffget_dir(), L"ffmpeg-8.1.1-full_build-shared\\LICENSE");
    bool fetched = GetFileAttributesW(licence.s) != INVALID_FILE_ATTRIBUTES;
    f32 lic_w = text_w(g_font, "Licence") + dp(32);
    opt_row_begin("FFmpeg 8.1.1 (gyan.dev build), GPL", fetched ? "Records and saves the video" : "Records and saves the video. This one is yours, not the one " APP_NAME " fetches",
                  fetched ? lic_w : 0);
    if (fetched && kit_button("Licence##fflicence", KIT_SECONDARY, ImVec2(lic_w, 0)))
        ShellExecuteW(0, L"open", L"notepad.exe", licence.s, 0, SW_SHOWNORMAL);
    opt_row_end();
    opt_card_end(0);
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
        // Apps opens on New when something's waiting to be sorted, else on what's recorded
        bool any_new = false;
        for (i32 i = 0; i < g_app.config.app_count; i++)
            any_new |= app_category(g_app.config, g_app.config.apps[i]) == CAT_NONE;
        if (o.filter == FILTER_NEW && !any_new) o.filter = FILTER_INCLUDED;
        if (o.filter == FILTER_INCLUDED && any_new) o.filter = FILTER_NEW;
    }
    o.last_frame = frame;

    // The sections as an icon and a label over a hairline, the current one underlined in the accent. Each tab is
    // as wide as its bold label, so none moves; the new-apps count sits on the Apps icon and takes no room.
    static const char *names[OPT_TAB_COUNT] = {"Apps", "Recording", "Hotkeys", "Integrations", "General"};
    static const char *icons[OPT_TAB_COUNT] = {ICON_APPS, ICON_VIDEO, ICON_KEYBOARD, ICON_LINK, ICON_SLIDERS};
    if (o.tab < 0 || o.tab >= OPT_TAB_COUNT) o.tab = OPT_APPS;
    i32 new_apps = 0;
    for (i32 i = 0; i < g_app.config.app_count; i++) // as the New chip counts: not ones a folder rule sorts
        new_apps += app_category(g_app.config, g_app.config.apps[i]) == CAT_NONE;
    char count[8];
    snprintf(count, sizeof count, new_apps > 99 ? "99+" : "%d", new_apps);

    // Every tab is one column of the same width, in the middle of the window
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 avail = ImGui::GetContentRegionAvail().x, tab_h = dp(34), gap = dp(28);
    f32 col_w = fminf(avail, dp(1080));
    a.x = floorf(a.x + (avail - col_w) / 2);
    avail = col_w;
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
        u32 ink = active ? COL_BG : hot ? COL_TEXT : COL_DIM;
        f32 iy = floorf(a.y + (tab_h - dp(4) - g_font_icon->FontSize) / 2);
        if (active) // the current tab: a white pill
            dl->AddRectFilled(ImVec2(x - dp(12), a.y), ImVec2(x + w + dp(12), a.y + tab_h - dp(4)), col32(COL_WHITE),
                              (tab_h - dp(4)) / 2);
        opt_text_at(dl, g_font_icon, ImVec2(x, iy), ink, icons[t], x + slot);
        opt_text_at(dl, font, ImVec2(x + slot, ty), ink, names[t], x + w + 1);
        if (t == OPT_APPS && new_apps) { // a badge on the icon's top right corner, ringed in the page colour
            f32 pw = opt_pill_w(count), ph = dp(18), r = dp(2);
            ImVec2 p(x + g_font_icon->FontSize - dp(9), iy);
            dl->AddRectFilled(ImVec2(p.x - r, p.y - ph / 2 - r), ImVec2(p.x + pw + r, p.y + ph / 2 + r), col32(COL_BG),
                              ph / 2 + r);
            opt_pill(dl, p, count, COL_WHITE, COL_BG);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        x += w + gap;
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + tab_h));
    ImGui::Dummy(ImVec2(avail, dp(6)));

    // List pages fit their page and turn pages; the others scroll if the window is short. A scrollbar goes to the
    // right of the column, so the cards keep the column's width (whether it scrolls is known from the last frame).
    bool list = o.tab == OPT_APPS || o.tab == OPT_HOTKEYS;
    g_combo_wheel = list;
    static bool scrolls;
    f32 bar = !list && scrolls ? ImGui::GetStyle().ScrollbarSize : 0;
    ImGui::SetCursorScreenPos(ImVec2(a.x, ImGui::GetCursorScreenPos().y));
    ImGui::BeginChild("page", ImVec2(col_w + bar, 0), ImGuiChildFlags_None,
                      list ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
    switch (o.tab) {
    case OPT_APPS: options_apps(); break;
    case OPT_RECORDING: options_recording(); break;
    case OPT_HOTKEYS: options_hotkeys(); break;
    case OPT_INTEGRATIONS: options_integrations(); break;
    case OPT_GENERAL: options_general(); break;
    default: break;
    }
    scrolls = ImGui::GetScrollMaxY() > 0;
    ImGui::EndChild();
    if (o.capture_slot) ui_wake(); // keep drawing while waiting for keys
}
