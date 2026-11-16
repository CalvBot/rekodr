// Apps' exes and icons, for every screen: where an app's exe is, showing a file in Explorer, and the exe's icon
// as a texture.

// Finding an app's exe for "Open file location": a running copy, else lifelog's record of where it saw it
// (%LOCALAPPDATA%\lifelog\paths.txt, "exe<TAB>path" lines; read only).

static bool app_find_path(const char *exe, Path *out)
{
    out->s[0] = 0;
    wchar_t want[MAX_PATH];
    utf8_to_wide(exe, want, MAX_PATH);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W e = {};
        e.dwSize = sizeof e;
        for (BOOL ok = Process32FirstW(snap, &e); ok && !out->s[0]; ok = Process32NextW(snap, &e)) {
            if (_wcsicmp(e.szExeFile, want)) continue;
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
            if (!p) continue; // elevated, or gone
            DWORD len = MAX_PATH;
            if (!QueryFullProcessImageNameW(p, 0, out->s, &len)) out->s[0] = 0;
            CloseHandle(p);
        }
        CloseHandle(snap);
    }
    if (out->s[0]) return true;

    wchar_t *local = 0;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, 0, &local);
    Path real = {}, paths = {};
    swprintf(real.s, MAX_PATH, L"%s\\lifelog\\paths.txt", local ? local : L".");
    CoTaskMemFree(local);
    import_path(L"lifelog-paths.txt", real.s, &paths);
    i64 size = 0;
    char *text = options_read_file(paths, &size);
    if (!text) return false;
    char *next = 0;
    for (char *line = strtok_s(text, "\r\n", &next); line && !out->s[0]; line = strtok_s(0, "\r\n", &next)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (_stricmp(line, exe)) continue;
        utf8_to_wide(tab + 1, out->s, MAX_PATH);
        if (GetFileAttributesW(out->s) == INVALID_FILE_ATTRIBUTES) out->s[0] = 0; // moved or uninstalled
    }
    free(text);
    return out->s[0] != 0;
}

// explorer /select, as the Rust version did (SHOpenFolderAndSelectItems returned E_ABORT from here).
static void reveal_file(const Path &path)
{
    wchar_t args[MAX_PATH + 16];
    swprintf(args, COUNT(args), L"/select,\"%s\"", path.s);
    INT_PTR r = (INT_PTR)ShellExecuteW(0, L"open", L"explorer.exe", args, 0, SW_SHOWNORMAL);
    char u[MAX_PATH * 3];
    wide_to_utf8(path.s, u, sizeof u);
    if (r <= 32) log_line("couldn't show %s in Explorer (%d)", u, (int)r);
    else log_line("showing %s in Explorer", u);
}

// ---------------------------------------------------------------------------------------------------------------
// App icons: the exe's own icon, found where "Open file location" finds the exe (a running copy, or lifelog's
// record). One is looked up per frame; an app with none found gets a letter tile until Options is entered again.

struct AppIcon {
    char exe[64];
    ID3D11ShaderResourceView *srv;
    bool tried;
};
static AppIcon g_app_icons[192];
static i32 g_app_icon_count;
static i32 g_app_icon_frame; // the frame that last looked one up

static ID3D11ShaderResourceView *app_icon_load(const Path &path)
{
    const i32 S = 48;
    HICON icon = 0;
    if (SHDefExtractIconW(path.s, 0, 0, &icon, 0, S) != S_OK || !icon) return 0;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S; // top down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = 0;
    HDC dc = CreateCompatibleDC(0);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    ID3D11ShaderResourceView *srv = 0;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        memset(bits, 0, S * S * 4);
        DrawIconEx(dc, 0, 0, icon, S, S, 0, 0, DI_NORMAL);
        SelectObject(dc, old);
        u32 *px = (u32 *)bits;
        bool alpha = false;
        for (i32 i = 0; i < S * S && !alpha; i++) alpha = (px[i] >> 24) != 0;
        if (!alpha) // an old icon without alpha: whatever was drawn is opaque
            for (i32 i = 0; i < S * S; i++)
                if (px[i] & 0xffffff) px[i] |= 0xff000000u;
        srv = gpu_texture(px, S, S);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc);
    DestroyIcon(icon);
    return srv;
}

static ID3D11ShaderResourceView *app_icon(const char *exe)
{
    AppIcon *slot = 0;
    for (i32 i = 0; i < g_app_icon_count; i++)
        if (str_eq(g_app_icons[i].exe, exe)) { slot = &g_app_icons[i]; break; }
    if (!slot) {
        if (g_app_icon_count >= COUNT(g_app_icons)) return 0;
        slot = &g_app_icons[g_app_icon_count++];
        *slot = {};
        str_set(slot->exe, exe);
    }
    if (slot->srv || slot->tried) return slot->srv;
    i32 frame = ImGui::GetFrameCount();
    if (frame == g_app_icon_frame) { ui_wake(); return 0; } // one per frame: the next frame does the next
    g_app_icon_frame = frame;
    slot->tried = true;
    Path p = {};
    if (app_find_path(exe, &p)) slot->srv = app_icon_load(p);
    ui_wake();
    return slot->srv;
}

// A game known only by its name ("Rust", from a clip): the icon of the installed program with that name, from
// Windows' list of installed programs (Steam games are in it, each with its icon). Letters and digits are all
// that's compared, so "Overwatch(R)" is Overwatch; a longer name is also found inside an installed one ("Rainbow
// Six Siege" in "Tom Clancy's Rainbow Six Siege"), but a short one never is ("Rust" isn't RustDesk).
static void name_letters(const wchar_t *in, wchar_t *out, i32 cap)
{
    i32 n = 0;
    for (; *in && n < cap - 1; in++)
        if (iswalnum(*in) && *in < 128) out[n++] = (wchar_t)towlower(*in);
    out[n] = 0;
}

static bool installed_icon_path(const char *game, Path *out)
{
    out->s[0] = 0;
    wchar_t wide[128], want[128];
    utf8_to_wide(game, wide, COUNT(wide));
    name_letters(wide, want, COUNT(want));
    if (!want[0]) return false;
    static const struct { HKEY root; const wchar_t *path; } places[] = {
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
    };
    bool exact = false;
    for (i32 k = 0; k < COUNT(places) && !exact; k++) {
        HKEY key;
        if (RegOpenKeyExW(places[k].root, places[k].path, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        wchar_t sub[256];
        for (DWORD i = 0; !exact; i++) {
            DWORD len = COUNT(sub);
            if (RegEnumKeyExW(key, i, sub, &len, 0, 0, 0, 0) != ERROR_SUCCESS) break;
            wchar_t name[256], letters[256], icon[MAX_PATH];
            DWORD bytes = sizeof name;
            if (RegGetValueW(key, sub, L"DisplayName", RRF_RT_REG_SZ, 0, name, &bytes) != ERROR_SUCCESS) continue;
            name_letters(name, letters, COUNT(letters));
            bool same = !wcscmp(letters, want), inside = !same && wcslen(want) >= 8 && wcsstr(letters, want);
            if (!same && !(inside && !out->s[0])) continue;
            bytes = sizeof icon;
            if (RegGetValueW(key, sub, L"DisplayIcon", RRF_RT_REG_SZ, 0, icon, &bytes) != ERROR_SUCCESS || !icon[0]) continue;
            // "C:\path\file.exe",0 -> C:\path\file.exe
            wchar_t *from = icon[0] == L'"' ? icon + 1 : icon;
            if (wchar_t *q = wcschr(from, L'"')) *q = 0;
            if (wchar_t *comma = wcsrchr(from, L','))
                if (comma > from + 3 && (iswdigit(comma[1]) || comma[1] == L'-')) *comma = 0;
            if (GetFileAttributesW(from) == INVALID_FILE_ATTRIBUTES) continue;
            swprintf(out->s, MAX_PATH, L"%s", from);
            exact = same;
        }
        RegCloseKey(key);
    }
    return out->s[0] != 0;
}

// The icon for a game's name: the app clipback knows by that name, else the installed program's. Null if neither
// has one (and while it is being looked up: one a frame, like app_icon).
static AppIcon g_game_icons[LIB_MAX_GAMES];
static i32 g_game_icon_count;

static ID3D11ShaderResourceView *game_icon(const char *name)
{
    AppIcon *slot = 0;
    for (i32 i = 0; i < g_game_icon_count; i++)
        if (str_eq(g_game_icons[i].exe, name)) { slot = &g_game_icons[i]; break; }
    if (!slot) {
        if (g_game_icon_count >= COUNT(g_game_icons)) return 0;
        slot = &g_game_icons[g_game_icon_count++];
        *slot = {};
        str_set(slot->exe, name);
    }
    if (slot->srv || slot->tried) return slot->srv;
    i32 frame = ImGui::GetFrameCount();
    if (frame == g_app_icon_frame) { ui_wake(); return 0; }
    g_app_icon_frame = frame;
    slot->tried = true;
    const Config &c = g_app.config;
    Path p = {};
    for (i32 i = 0; i < c.app_count && !p.s[0]; i++) {
        char shown[128];
        audio_display_name(c, c.apps[i].exe.s, shown, sizeof shown);
        if (!_stricmp(shown, name) && !app_find_path(c.apps[i].exe.s, &p)) p.s[0] = 0;
    }
    if (p.s[0] || installed_icon_path(name, &p)) slot->srv = app_icon_load(p);
    ui_wake();
    return slot->srv;
}

// Entering Options again tries the ones that had no icon (a game may be running now).
static void app_icons_retry()
{
    for (i32 i = 0; i < g_app_icon_count; i++)
        if (!g_app_icons[i].srv) g_app_icons[i].tried = false;
}
