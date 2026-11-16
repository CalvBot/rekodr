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

// Entering Options again tries the ones that had no icon (a game may be running now).
static void app_icons_retry()
{
    for (i32 i = 0; i < g_app_icon_count; i++)
        if (!g_app_icons[i].srv) g_app_icons[i].tried = false;
}
