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

    // lifelog's list of where each exe it saw lives (a test copy reads <home>\\import\\lifelog-paths.txt instead)
    Path paths = {};
    wchar_t home[MAX_PATH] = {}, *local = 0;
    if (GetEnvironmentVariableW(L"REKODR_HOME", home, MAX_PATH) && home[0]) {
        swprintf(paths.s, MAX_PATH, L"%s\\import\\lifelog-paths.txt", home);
    } else {
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, 0, &local);
        swprintf(paths.s, MAX_PATH, L"%s\\lifelog\\paths.txt", local ? local : L".");
        CoTaskMemFree(local);
    }
    i64 size = 0;
    char *text = read_file(paths.s, &size);
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

// explorer /select (SHOpenFolderAndSelectItems returns E_ABORT from here).
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
    char exe[64]; // or a game's name
    ID3D11ShaderResourceView *srv;
    bool tried;
    bool waiting; // its art is being fetched: tried again when it arrives
    i32  w, h;    // a cover's size, to crop it to where it is drawn
    ID3D11ShaderResourceView *blur; // a cover, blurred: for behind text
};
static AppIcon g_app_icons[192];
static i32 g_app_icon_count;
static i32 g_app_icon_frame; // the frame that last looked one up

// A program's icon, APP_ICON_PX square, as straight-alpha BGRA (HeapAlloc'd). Null if it has none.
static const i32 APP_ICON_PX = 48;
static u32 *app_icon_pixels(const Path &path)
{
    const i32 S = APP_ICON_PX;
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
    u32 *out = 0;
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
        out = (u32 *)HeapAlloc(GetProcessHeap(), 0, S * S * 4);
        if (out) memcpy(out, px, S * S * 4);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc);
    DestroyIcon(icon);
    return out;
}

static ID3D11ShaderResourceView *app_icon_load(const Path &path)
{
    u32 *px = app_icon_pixels(path);
    if (!px) return 0;
    ID3D11ShaderResourceView *srv = gpu_texture(px, APP_ICON_PX, APP_ICON_PX);
    HeapFree(GetProcessHeap(), 0, px);
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

// A picture file (PNG or JPEG) as a texture. Main thread.
static u32 *wic_load(IWICImagingFactory *wic, const wchar_t *path, i32 *w, i32 *h); // library.cpp

// A picture blurred, as a texture: it is shrunk to 160 across, or 128 down (each pixel the average of the ones it covers) and
// smoothed, and stretching that back out when it is drawn does the rest.
static ID3D11ShaderResourceView *blurred_texture(const u32 *pixels, i32 w, i32 h)
{
    i32 bw = 160, bh = h * bw / w;
    if (bh > 128) { // a square or tall one: 128 down instead
        bh = 128;
        bw = w * bh / h;
    }
    if (bw < 4 || bh < 4 || w < bw || h < bh) return 0;
    static u32 shrunk[160 * 128], pass[160 * 128];
    for (i32 y = 0; y < bh; y++) {
        for (i32 x = 0; x < bw; x++) {
            i32 x0 = x * w / bw, x1 = (x + 1) * w / bw, y0 = y * h / bh, y1 = (y + 1) * h / bh;
            u64 b = 0, g = 0, r = 0, n = 0;
            for (i32 sy = y0; sy < y1; sy += 2) // every other pixel is plenty for an average
                for (i32 sx = x0; sx < x1; sx += 2) {
                    u32 p = pixels[sy * w + sx];
                    b += p & 255; g += (p >> 8) & 255; r += (p >> 16) & 255; n++;
                }
            if (!n) n = 1;
            shrunk[y * bw + x] = 0xff000000u | (u32)(r / n) << 16 | (u32)(g / n) << 8 | (u32)(b / n);
        }
    }
    for (i32 round = 0; round < 2; round++) { // each pixel with its eight neighbours, twice over
        for (i32 y = 0; y < bh; y++) {
            for (i32 x = 0; x < bw; x++) {
                u32 b = 0, g = 0, r = 0, n = 0;
                for (i32 dy = -1; dy <= 1; dy++)
                    for (i32 dx = -1; dx <= 1; dx++) {
                        i32 sx = x + dx, sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= bw || sy >= bh) continue;
                        u32 p = shrunk[sy * bw + sx];
                        b += p & 255; g += (p >> 8) & 255; r += (p >> 16) & 255; n++;
                    }
                pass[y * bw + x] = 0xff000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
            }
        }
        memcpy(shrunk, pass, (size_t)bw * bh * 4);
    }
    return gpu_texture(shrunk, bw, bh);
}

// `max_side` halves the picture (each pixel the average of four) until it is no bigger than that: an icon drawn
// much smaller than it is stored would otherwise shimmer, since these textures have no smaller copies of themselves.
static ID3D11ShaderResourceView *image_file_texture(const Path &file, i32 *out_w = 0, i32 *out_h = 0,
                                                    ID3D11ShaderResourceView **blur = 0, i32 max_side = 0)
{
    static IWICImagingFactory *wic;
    if (!wic && FAILED(CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return 0;
    i32 w = 0, h = 0;
    u32 *pixels = wic_load(wic, file.s, &w, &h);
    if (!pixels) return 0;
    while (max_side > 0 && (w > max_side || h > max_side) && w >= 2 && h >= 2) {
        i32 hw = w / 2, hh = h / 2;
        for (i32 y = 0; y < hh; y++)
            for (i32 x = 0; x < hw; x++) {
                const u32 *q = pixels + (y * 2) * w + x * 2;
                u32 p[4] = {q[0], q[1], q[w], q[w + 1]}, out = 0;
                for (i32 c = 0; c < 32; c += 8) out |= ((((p[0] >> c) & 255) + ((p[1] >> c) & 255) + ((p[2] >> c) & 255) + ((p[3] >> c) & 255) + 2) / 4) << c;
                pixels[y * hw + x] = out;
            }
        w = hw;
        h = hh;
    }
    ID3D11ShaderResourceView *srv = gpu_texture(pixels, w, h);
    if (blur) *blur = blurred_texture(pixels, w, h);
    HeapFree(GetProcessHeap(), 0, pixels);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return srv;
}

static i32 g_icon_lookups; // this frame's; one a frame, so a screen full of new icons doesn't stall it
static bool icon_lookup_ok()
{
    i32 frame = ImGui::GetFrameCount();
    if (frame == g_app_icon_frame) { ui_wake(); return false; }
    g_app_icon_frame = frame;
    return true;
}

static AppIcon *icon_slot(AppIcon *slots, i32 *count, i32 cap, const char *key)
{
    for (i32 i = 0; i < *count; i++)
        if (str_eq(slots[i].exe, key)) return &slots[i];
    if (*count >= cap) return 0;
    AppIcon *slot = &slots[(*count)++];
    *slot = {};
    str_set(slot->exe, key);
    return slot;
}

// A game's icon from the games database, by the game's name. Null if it has none, and while it is being fetched.
static AppIcon g_db_icons[LIB_MAX_GAMES + 64];
static i32 g_db_icon_count;

// GAME_DESKTOP and GAME_NOT_OURS aren't games, so no lookup gives them an icon
static bool game_is_none(const char *name) { return !strcmp(name, GAME_DESKTOP) || !strcmp(name, GAME_NOT_OURS); }

static ID3D11ShaderResourceView *game_db_icon(const char *name)
{
    if (game_is_none(name) || game_is_custom(name)) return 0;
    AppIcon *slot = icon_slot(g_db_icons, &g_db_icon_count, COUNT(g_db_icons), name);
    if (!slot) return 0;
    if (slot->srv || slot->tried) return slot->srv;
    if (!icon_lookup_ok()) return 0;
    slot->tried = true;
    slot->waiting = false;
    GameInfo g;
    Path p = {};
    if (!gamedb_for_art(name, &g)) return 0;
    if (gamedb_art(g, ART_ICON, &p, &slot->waiting)) slot->srv = image_file_texture(p, 0, 0, 0, 64);
    ui_wake();
    return slot->srv;
}

// A game's cover from the games database, by the game's name, its size, and a blurred copy of it. Null if it has
// none, and while it is being fetched.
static AppIcon g_db_covers[LIB_MAX_GAMES];
static i32 g_db_cover_count;

static ID3D11ShaderResourceView *game_db_cover(const char *name, i32 *w, i32 *h, ID3D11ShaderResourceView **blur = 0)
{
    if (game_is_custom(name)) return 0;
    AppIcon *slot = icon_slot(g_db_covers, &g_db_cover_count, COUNT(g_db_covers), name);
    if (!slot) return 0;
    if (!slot->srv && !slot->tried && icon_lookup_ok()) {
        slot->tried = true;
        slot->waiting = false;
        GameInfo g;
        Path p = {};
        if (gamedb_for_art(name, &g) && gamedb_art(g, ART_COVER, &p, &slot->waiting))
            slot->srv = image_file_texture(p, &slot->w, &slot->h, &slot->blur);
        ui_wake();
    }
    *w = slot->w;
    *h = slot->h;
    if (blur) *blur = slot->blur;
    return slot->srv;
}

// Box art picked by hand for the cards of Options > Auto markers, Steam achievements too: SteamGridDB grids
// (600 x 900), the main one first, then two backups taken in turn when the one before can't be had. Fetched from
// SteamGridDB's own address by a worker thread, once a start, into gamedb\art\grid-<file>; one fetched before is used
// as it is.
struct FixedCover { const char *game; const char *grid[3]; };
static const FixedCover g_fixed_covers[] = {
    {"Counter-Strike 2", {"0662aa1719017e0efa5fa8daf0880c6e.png", "96b4fb6dee79d9f1d0703e15a13c58a1.jpg", "7c1a285fb1e76489437c3c780dd8dc85.png"}},
    {"League of Legends", {"8e65aff4b74febf52c314c22e34c2d32.png", "739ba8f780c1b9a7bc5e17d61cb8cb41.png", "cf29ee336b07c0572c810f62f5543525.png"}},
    {"Dota 2", {"f92e37859ddb71005399deb36970c8fb.jpg", "d19fd02ced4e8e9ccc0994d8dfee171f.png", "11cb2bd8917c92619467198cdec4f708.png"}},
    {"Rocket League", {"e4e4005042cf598805d581754fe9256f.png", "a94595e22a11a0e271943269b1605e8d.png", "5fe5b01f95f050c99c170481f6dc88fb.png"}},
    {"War Thunder", {"f69aa2b8ef6276214e009a37a7914043.jpg", "5d8ad96f48f8cada0a4d6564d7ece178.png", "f1eb69485d261b0d71297d3b7a617ce6.png"}},
    {"F1 25", {"fb9d97ffedbaa16cae0906034a254afd.png", "da8f17df33b392e61af87eedc7a02773.png", "8eb9469f949e95b6574695b77ee2b9ab.jpg"}},
    {"iRacing", {"22b7de72c07203f1425e06b010bbe95c.png", "3498e9a5704fcc4e89c62455d9d020e5.png", "489d3e1fa4d57d880d7e88a3f3cc9f85.png"}},
    {"Assetto Corsa Competizione", {"efaa0ec2e8906a9e6e6988ec6bf3a658.png", "708250b08aff2266b6c3d6df7ab6b2bb.png", "a257950fdafaaa29c59c853567aae00b.png"}},
    {"Deadlock", {"267d0674eaa904611272755c4d16c0f9.png", "f27f07335126e279da5d9b411265ec29.png", "ddd7c343a60f163d840180440802b280.png"}},
    {"Minecraft", {"a73027901f88055aaa0fd1a9e25d36c7.png", "ee897a8699b37568c5c8dd72f78c2bf1.png", "7344bcd24a4a0393f45cad1a212c62e7.png"}},
    {"Team Fortress 2", {"139b23924c6bb8418a337b563518d83a.jpg", "41dfccda9b5b255be17050bf5fb959f3.png", "22de0bf85202126667cb67ede1da08b7.png"}},
    {"Path of Exile", {"b18f9c54e6efde8a08b8b478b0806587.png", "b762ab8b6d4b6845861b3a7fc6ed5a91.png", "e47278fe2669e7b284c7b75c76f02b12.jpg"}},
    {"Path of Exile 2", {"579b8ed826a6b614ac5654924627477d.jpg", "602dc7261c04c9ae1d85e4ef3d191f0a.png", "494b08f5ba3e464e99d3ffa5ed13535a.jpg"}},
    {"Hearthstone", {"f30259f76102eefd8b482ee4b68b1ef7.png", "3128463b233d51bd542a59d9cf49a5f6.jpg", "fb50a80a148e6facff6f7bb1241180ad.png"}},
    {"Steam achievements", {"39c2966989c4f0091a99eef7f1d09c09.png", "c5174327c1975f78b7ffc788ed60b80e.png", "dba324d3fb4b6d4ddd559fb5a70ff362.jpg"}},
};
enum { GAMEDB_FIXED_COVER = 5 }; // gamedb_on_event: the worker has settled one of the hand-picked covers
// Per cover: which grid is there (1-3), 0 while not settled, -1 when none could be had.
static struct {
    volatile LONG started;
    volatile LONG got[COUNT(g_fixed_covers)];
    AppIcon slot[COUNT(g_fixed_covers)];
} g_fixed;

static Path fixed_cover_file(const char *grid)
{
    wchar_t name[96];
    swprintf(name, COUNT(name), L"art\\grid-%hs", grid);
    return path_join(gamedb_dir(), name);
}

static DWORD WINAPI fixed_covers_worker(void *)
{
    for (i32 i = 0; i < COUNT(g_fixed_covers); i++) {
        LONG got = -1;
        for (i32 k = 0; k < 3 && got < 0; k++) {
            Path p = fixed_cover_file(g_fixed_covers[i].grid[k]);
            WIN32_FILE_ATTRIBUTE_DATA fa;
            bool have = GetFileAttributesExW(p.s, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 0;
            char url[128];
            snprintf(url, sizeof url, "https://cdn2.steamgriddb.com/grid/%s", g_fixed_covers[i].grid[k]);
            if (have || (g_gamedb.online && http_get_url(url, p))) got = k + 1;
            else DEV_LOG("gamedb: cover %s for %s didn't come", g_fixed_covers[i].grid[k], g_fixed_covers[i].game);
        }
        InterlockedExchange(&g_fixed.got[i], got);
        Event e = {};
        e.kind = EV_GAMEDB;
        e.a = GAMEDB_FIXED_COVER;
        event_post(e);
    }
    return 0;
}

// A game's hand-picked cover (g_fixed_covers) and its size. *fixed is false for a game without one, or when none of
// its grids could be had; with it true, null means the cover is still coming.
static ID3D11ShaderResourceView *game_fixed_cover(const char *name, i32 *w, i32 *h, bool *fixed)
{
    *fixed = false;
    i32 i = 0;
    while (i < COUNT(g_fixed_covers) && strcmp(g_fixed_covers[i].game, name)) i++;
    if (i == COUNT(g_fixed_covers)) return 0;
    if (!InterlockedExchange(&g_fixed.started, 1)) {
        HANDLE t = CreateThread(0, 0, fixed_covers_worker, 0, 0, 0);
        if (t) CloseHandle(t);
    }
    LONG got = g_fixed.got[i];
    if (got < 0) return 0;
    *fixed = true;
    AppIcon *slot = &g_fixed.slot[i];
    if (got > 0 && !slot->srv && !slot->tried) {
        slot->tried = true;
        slot->srv = image_file_texture(fixed_cover_file(g_fixed_covers[i].grid[got - 1]), &slot->w, &slot->h);
        if (!slot->srv) *fixed = false;
    }
    if (slot->tried && !slot->srv) *fixed = false;
    *w = slot->w;
    *h = slot->h;
    return slot->srv;
}

// A game's art for behind the window (ui_art_texture) and the toasts (game_art_picture): its SteamGridDB hero, else
// its grid (gamedb_art's ART_COVER), in grey, shrunk to g_art_look.w across, Gaussian blurred (g_art_look.blur
// texels) until only its big light and dark masses are left, and its levels stretched so its darkest and lightest
// parts reach black and white. Stretched over the window, drawing it smooths between the texels. Its colour, shrunk
// and blurred the same, is kept beside it for the toasts, which show some of it (g_art_look.toast_colour); the window
// is grey. A worker thread makes one for every game whose art is there, at start and as art arrives, the Library's
// games first, and keeps it in local_dir\arttex\<game id>.grey, so a later start only reads it back.
struct ArtLook {
    i32 w;          // texels across
    f32 blur;       // Gaussian sigma, in texels
    f32 opacity;    // of the art over the ground, at its right edge (art_fade)
    f32 fade_end;   // how far in from the right edge, as a share of the width, the art has faded out to the ground
    f32 toast_fade_end; // the same on the toasts
    f32 toast_colour;   // how much of the art's colour the toasts show: 0 grey, 1 all of it
    f32 grain;      // of the grain over the art
};
static constexpr ArtLook g_art_look = {192, 1.2f, 0.26f, 0.5f, 1.0f, 0.5f, 0.025f};
// The tallest kept is 5/8 of the width; a taller picture is cut to its middle. A picture of w x h texels is
// ART_PLANES planes of w x h bytes: the grey, then the colour's blue, green and red.
enum { ART_W_MAX = 192, ART_H_MAX = ART_W_MAX * 5 / 8, ART_PLANES = 4 };
static_assert(g_art_look.w <= ART_W_MAX);

static i32 art_clamp(i32 v, i32 lo, i32 hi) { return v < lo ? lo : v > hi ? hi : v; }

enum ArtState : u8 { ART_NEW, ART_QUEUED, ART_MAKING, ART_MADE, ART_DONE };
struct ArtTexture {
    char      name[64];
    ArtState  state;   // NEW: not looked at; QUEUED/MAKING: with the worker; MADE: `grey` waits to be uploaded
    bool      waiting; // its art is being fetched: looked at again when it arrives
    i32       w, h;
    u8       *grey;    // MADE: the worker's picture's grey, w x h (heap)
    Path      art, cache;
    ID3D11ShaderResourceView *srv;
};
static ArtTexture g_art[LIB_MAX_GAMES];
static i32 g_art_count;
static struct {
    SRWLOCK lock; // over the slots' state, w, h and grey
    HANDLE  wake, thread;
    u64     games_hash; // of the Library's games last asked for
    bool    dirty;      // the games list or art changed: ask again
    bool    sweep;      // under the lock: the worker is to look through every game's art
    Path    art_dir, cache_dir;
} g_art_worker;

// The look's key, kept in each cached file: a file made with another look is made again.
static u32 art_look_key() { return (u32)g_art_look.w * 1000u + (u32)(g_art_look.blur * 100.0f + 0.5f); }

// The art file as the picture, into `pic` (ART_PLANES x ART_W_MAX x ART_H_MAX). Worker thread.
static bool art_grey_make(IWICImagingFactory *wic, const Path &file, u8 *pic, i32 *out_w, i32 *out_h)
{
    i32 w = 0, h = 0;
    u32 *pixels = wic_load(wic, file.s, &w, &h);
    if (!pixels) return false;
    i32 bw = g_art_look.w, bh = h * bw / (w > 0 ? w : 1);
    if (bh > bw * 5 / 8) bh = bw * 5 / 8;
    if (bh < 4 || w < bw || h < bh) {
        HeapFree(GetProcessHeap(), 0, pixels);
        return false;
    }
    static f32 a[ART_PLANES][ART_W_MAX * ART_H_MAX], b[ART_W_MAX * ART_H_MAX];
    // Each texel the average brightness and colour of the pixels it covers
    i32 used_h = (i32)((i64)w * bh / bw), top = (h - used_h) / 2;
    if (used_h > h) used_h = h, top = 0;
    for (i32 y = 0; y < bh; y++)
        for (i32 x = 0; x < bw; x++) {
            i32 x0 = x * w / bw, x1 = (x + 1) * w / bw, y0 = top + y * used_h / bh, y1 = top + (y + 1) * used_h / bh;
            u64 sum[ART_PLANES] = {}, n = 0;
            for (i32 sy = y0; sy < y1; sy++)
                for (i32 sx = x0; sx < x1; sx++) {
                    u32 p = pixels[sy * w + sx];
                    sum[0] += (((p >> 16) & 255) * 54 + ((p >> 8) & 255) * 183 + (p & 255) * 19) >> 8;
                    for (i32 k = 1; k < ART_PLANES; k++) sum[k] += (p >> (8 * (k - 1))) & 255;
                    n++;
                }
            for (i32 k = 0; k < ART_PLANES; k++) a[k][y * bw + x] = n ? (f32)sum[k] / n : 0;
        }
    HeapFree(GetProcessHeap(), 0, pixels);
    // Gaussian blur, across then down, the edges held
    f32 kernel[64];
    i32 r = (i32)(g_art_look.blur * 3.0f + 0.5f);
    if (r > 31) r = 31;
    f32 ksum = 0;
    for (i32 i = -r; i <= r; i++) ksum += kernel[i + r] = expf(-(f32)(i * i) / (2 * g_art_look.blur * g_art_look.blur + 1e-6f));
    for (i32 i = 0; i <= 2 * r; i++) kernel[i] /= ksum;
    for (i32 k = 0; k < ART_PLANES; k++)
        for (i32 dir = 0; dir < 2; dir++) {
            f32 *from = dir ? b : a[k], *to = dir ? a[k] : b;
            for (i32 y = 0; y < bh; y++)
                for (i32 x = 0; x < bw; x++) {
                    f32 v = 0;
                    for (i32 d = -r; d <= r; d++) {
                        i32 sx = dir ? x : art_clamp(x + d, 0, bw - 1), sy = dir ? art_clamp(y + d, 0, bh - 1) : y;
                        v += from[sy * bw + sx] * kernel[d + r];
                    }
                    to[y * bw + x] = v;
                }
        }
    // Levels: the darkest 2% to black, the lightest 2% to white (the grey; the colour is kept as it is)
    u32 hist[256] = {};
    for (i32 i = 0; i < bw * bh; i++) hist[art_clamp((i32)a[0][i], 0, 255)]++;
    i32 lo = 0, hi = 255, cut = bw * bh / 50;
    for (u32 n = 0; lo < 255 && (n += hist[lo]) <= (u32)cut;) lo++;
    for (u32 n = 0; hi > 0 && (n += hist[hi]) <= (u32)cut;) hi--;
    if (hi - lo < 16) hi = lo + 16;
    for (i32 i = 0; i < bw * bh; i++) pic[i] = (u8)art_clamp((i32)((a[0][i] - lo) * 255.0f / (hi - lo) + 0.5f), 0, 255);
    for (i32 k = 1; k < ART_PLANES; k++)
        for (i32 i = 0; i < bw * bh; i++) pic[k * bw * bh + i] = (u8)art_clamp((i32)(a[k][i] + 0.5f), 0, 255);
    *out_w = bw;
    *out_h = bh;
    return true;
}

struct ArtFileHead { u32 magic, key; u16 w, h; };
static const u32 ART_MAGIC = 0x54414243; // "CBAT"

// A cached picture, when it is newer than the art and made with this look.
static bool art_grey_read(const Path &cache, const Path &art, u8 *pic, i32 *w, i32 *h)
{
    WIN32_FILE_ATTRIBUTE_DATA fc, fa;
    if (!GetFileAttributesExW(cache.s, GetFileExInfoStandard, &fc) || !GetFileAttributesExW(art.s, GetFileExInfoStandard, &fa))
        return false;
    if (CompareFileTime(&fc.ftLastWriteTime, &fa.ftLastWriteTime) < 0) return false;
    FILE *f = _wfopen(cache.s, L"rb");
    if (!f) return false;
    ArtFileHead head = {};
    bool ok = fread(&head, sizeof head, 1, f) == 1 && head.magic == ART_MAGIC && head.key == art_look_key() &&
              head.w >= 4 && head.w <= ART_W_MAX && head.h >= 4 && head.h <= ART_H_MAX &&
              fread(pic, (size_t)ART_PLANES * head.w * head.h, 1, f) == 1;
    fclose(f);
    *w = head.w;
    *h = head.h;
    return ok;
}

static void art_grey_write(const Path &cache, const u8 *pic, i32 w, i32 h)
{
    Path tmp = cache;
    wcscat_s(tmp.s, L".part");
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) return;
    ArtFileHead head = {ART_MAGIC, art_look_key(), (u16)w, (u16)h};
    bool ok = fwrite(&head, sizeof head, 1, f) == 1 && fwrite(pic, (size_t)ART_PLANES * w * h, 1, f) == 1;
    ok &= fclose(f) == 0;
    if (!ok || !MoveFileExW(tmp.s, cache.s, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.s);
}

// The picture for an art file: the cached one, else made from the art and cached. Worker thread.
static bool art_grey_cached(IWICImagingFactory *wic, const Path &art, const Path &cache, u8 *pic, i32 *w, i32 *h)
{
    if (art_grey_read(cache, art, pic, w, h)) return true;
    if (!wic || !art_grey_make(wic, art, pic, w, h)) return false;
    art_grey_write(cache, pic, *w, *h);
    return true;
}

enum { GAMEDB_ART_TEXTURE = 4 }; // gamedb_on_event: a game's art texture has been made

static DWORD WINAPI art_worker(void *)
{
    CoInitializeEx(0, COINIT_MULTITHREADED);
    IWICImagingFactory *wic = 0;
    CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    static u8 pic[ART_PLANES * ART_W_MAX * ART_H_MAX];
    for (;;) {
        WaitForSingleObject(g_art_worker.wake, INFINITE);
        for (;;) {
            AcquireSRWLockExclusive(&g_art_worker.lock);
            ArtTexture *t = 0;
            for (i32 i = 0; i < g_art_count && !t; i++)
                if (g_art[i].state == ART_QUEUED) t = &g_art[i];
            if (t) t->state = ART_MAKING;
            ReleaseSRWLockExclusive(&g_art_worker.lock);
            if (!t) break;
            i32 w = 0, h = 0;
            bool ok = art_grey_cached(wic, t->art, t->cache, pic, &w, &h);
            u8 *copy = ok ? (u8 *)malloc((size_t)w * h) : 0; // the window's is grey
            if (copy) memcpy(copy, pic, (size_t)w * h);
            AcquireSRWLockExclusive(&g_art_worker.lock);
            t->grey = copy;
            t->w = w;
            t->h = h;
            t->state = copy ? ART_MADE : ART_DONE;
            ReleaseSRWLockExclusive(&g_art_worker.lock);
            Event e = {};
            e.kind = EV_GAMEDB;
            e.a = GAMEDB_ART_TEXTURE;
            event_post(e);
        }
        // Then every game's art there is, the Library's or not, so a game shown later has its texture ready
        AcquireSRWLockExclusive(&g_art_worker.lock);
        bool sweep = g_art_worker.sweep;
        g_art_worker.sweep = false;
        ReleaseSRWLockExclusive(&g_art_worker.lock);
        if (!sweep) continue;
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW(path_join(g_art_worker.art_dir, L"*-art.jpg").s, &fd);
        for (BOOL more = find != INVALID_HANDLE_VALUE; more; more = FindNextFileW(find, &fd)) {
            if (!fd.nFileSizeLow && !fd.nFileSizeHigh) continue; // none to be had
            wchar_t name[MAX_PATH];
            swprintf(name, COUNT(name), L"%s", fd.cFileName);
            *wcsstr(name, L"-art.jpg") = 0;
            wcscat_s(name, L".grey");
            i32 w = 0, h = 0;
            art_grey_cached(wic, path_join(g_art_worker.art_dir, fd.cFileName), path_join(g_art_worker.cache_dir, name),
                            pic, &w, &h);
        }
        if (find != INVALID_HANDLE_VALUE) FindClose(find);
    }
}

// Starts the worker on what is queued, and with `sweep` on every game's art as well. Main thread.
static void art_worker_wake(bool sweep)
{
    if (!g_art_worker.thread) {
        g_art_worker.art_dir = path_join(gamedb_dir(), L"art");
        g_art_worker.cache_dir = path_join(g_app.paths.local_dir, L"arttex");
        CreateDirectoryW(g_art_worker.cache_dir.s, 0);
        g_art_worker.wake = CreateEventW(0, FALSE, FALSE, 0);
        g_art_worker.thread = CreateThread(0, 0, art_worker, 0, 0, 0);
    }
    if (sweep) {
        AcquireSRWLockExclusive(&g_art_worker.lock);
        g_art_worker.sweep = true;
        ReleaseSRWLockExclusive(&g_art_worker.lock);
    }
    SetEvent(g_art_worker.wake);
}

// Where a game's grey picture is kept: local_dir\arttex\<game id>.grey
static Path art_cache_path(const GameInfo &g)
{
    wchar_t file[64];
    swprintf(file, COUNT(file), L"%hs.grey", g.id);
    return path_join(path_join(g_app.paths.local_dir, L"arttex"), file);
}

// A game's picture, the window's with its colour, for drawing without the GPU (the toasts): the worker's cached file
// read back, ART_PLANES planes of w x h bytes (malloc; the caller frees it). Null for a game with none, while it is
// being fetched or made, and with Flat look.
static u8 *game_art_picture(const char *name, i32 *w, i32 *h)
{
    if (g_app.config.flat_look || !name[0] || game_is_none(name) || game_is_custom(name)) return 0;
    GameInfo g;
    Path art = {};
    if (!gamedb_for_art(name, &g) || !gamedb_art(g, ART_COVER, &art)) return 0;
    u8 *pic = (u8 *)malloc(ART_PLANES * ART_W_MAX * ART_H_MAX);
    if (pic && art_grey_read(art_cache_path(g), art, pic, w, h)) return pic;
    free(pic);
    return 0;
}

// Asks for every Library game's art texture that isn't made yet: from the cache, else from its art once that is
// there. Main thread; cheap when nothing changed.
static void art_textures_update()
{
    if (g_app.config.flat_look) return;
    const Library &lib = g_app.library;
    u64 hash = fnv64(lib.games, sizeof lib.games[0] * (size_t)lib.game_count);
    if (!g_art_worker.dirty && hash == g_art_worker.games_hash) return;
    g_art_worker.dirty = false;
    g_art_worker.games_hash = hash;
    bool queued = false;
    for (i32 i = 0; i < lib.game_count; i++) {
        const char *name = lib.games[i];
        if (!name[0] || game_is_none(name) || game_is_custom(name)) continue;
        ArtTexture *t = 0;
        for (i32 k = 0; k < g_art_count && !t; k++)
            if (str_eq(g_art[k].name, name)) t = &g_art[k];
        if (!t) {
            if (g_art_count >= COUNT(g_art)) continue;
            AcquireSRWLockExclusive(&g_art_worker.lock);
            t = &g_art[g_art_count++];
            *t = {};
            str_set(t->name, name);
            ReleaseSRWLockExclusive(&g_art_worker.lock);
        }
        if (t->state != ART_NEW) continue; // only the main thread moves a slot on from NEW
        GameInfo g;
        Path p = {};
        t->waiting = false;
        if (!gamedb_for_art(name, &g) || !gamedb_art(g, ART_COVER, &p, &t->waiting)) {
            t->state = ART_DONE;
            continue;
        }
        AcquireSRWLockExclusive(&g_art_worker.lock);
        t->art = p;
        t->cache = art_cache_path(g);
        t->state = ART_QUEUED;
        ReleaseSRWLockExclusive(&g_art_worker.lock);
        queued = true;
    }
    if (queued) art_worker_wake(false);
}

// A game's art texture and its size, once made. Null if it has none, and while it is being fetched or made.
static ID3D11ShaderResourceView *game_art_texture(const char *name, i32 *w, i32 *h)
{
    art_textures_update();
    if (!name[0]) return 0;
    ArtTexture *t = 0;
    for (i32 k = 0; k < g_art_count && !t; k++)
        if (str_eq(g_art[k].name, name)) t = &g_art[k];
    if (!t) return 0;
    if (!t->srv) {
        AcquireSRWLockExclusive(&g_art_worker.lock);
        if (t->state == ART_MADE) {
            static u32 bgra[ART_W_MAX * ART_H_MAX];
            for (i32 i = 0; i < t->w * t->h; i++) bgra[i] = 0xff000000u | (u32)t->grey[i] * 0x010101u;
            t->srv = gpu_texture(bgra, t->w, t->h);
            free(t->grey);
            t->grey = 0;
            t->state = ART_DONE;
        }
        ReleaseSRWLockExclusive(&g_art_worker.lock);
    }
    *w = t->w;
    *h = t->h;
    return t->srv;
}

// The icon for a game's name: the database's, else the app Rekodr knows by that name, else the installed
// program's. Null if none has one, for a custom game (whose name may be any program's), and while it is being
// looked up (one a frame, like app_icon).
static AppIcon g_game_icons[LIB_MAX_GAMES];
static i32 g_game_icon_count;

static ID3D11ShaderResourceView *game_icon(const char *name)
{
    if (game_is_none(name) || game_is_custom(name)) return 0;
    if (ID3D11ShaderResourceView *art = game_db_icon(name)) return art;
    AppIcon *slot = icon_slot(g_game_icons, &g_game_icon_count, COUNT(g_game_icons), name);
    if (!slot) return 0;
    if (slot->srv || slot->tried) return slot->srv;
    if (!icon_lookup_ok()) return 0;
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

static void library_rescan(); // library.cpp

// The games list has loaded, art has arrived, or a modpack has been named. Main thread.
static void gamedb_on_event(const Event &e)
{
    if (e.a == GAMEDB_ART_TEXTURE || e.a == GAMEDB_FIXED_COVER) {
        ui_wake();
        return;
    }
    if (e.a == GAMEDB_LOADED || e.a == GAMEDB_RESOLVED) {
        Config &c = g_app.config;
        bool changed = false;
        for (i32 i = 0; i < c.app_count; i++) changed |= gamedb_name_app(&c.apps[i]);
        if (e.a == GAMEDB_LOADED) {
            log_line("games list: %lld games", e.b);
            // Videos Rekodr didn't save may now be placed by their names (game_guess)
            const Library &lib = g_app.library;
            bool other = false;
            for (i32 i = 0; i < lib.item_count && !other; i++) other = !strcmp(lib.items[i].info.game, GAME_NOT_OURS);
            if (other) library_rescan();
        }
        if (changed) {
            config_save(c, g_app.paths);
            audio_config_changed();
            g_app.options.order_dirty = true;
        }
    }
    if (e.a == GAMEDB_ART && e.b) { // the game called e.text has new art: its old pictures go
        auto drop = [](ID3D11ShaderResourceView *&srv) {
            if (srv) srv->Release();
            srv = 0;
        };
        for (i32 i = 0; i < g_db_icon_count; i++)
            if (str_eq(g_db_icons[i].exe, e.text)) {
                drop(g_db_icons[i].srv);
                g_db_icons[i].tried = false;
            }
        for (i32 i = 0; i < g_db_cover_count; i++)
            if (str_eq(g_db_covers[i].exe, e.text)) {
                drop(g_db_covers[i].srv);
                drop(g_db_covers[i].blur);
                g_db_covers[i].tried = false;
            }
        AcquireSRWLockExclusive(&g_art_worker.lock);
        for (i32 i = 0; i < g_art_count; i++)
            if (str_eq(g_art[i].name, e.text) && (g_art[i].state == ART_DONE || g_art[i].state == ART_NEW)) {
                drop(g_art[i].srv);
                g_art[i].state = ART_NEW;
                g_art_worker.dirty = true;
            }
        ReleaseSRWLockExclusive(&g_art_worker.lock);
    }
    for (i32 i = 0; i < g_db_icon_count; i++)
        if (!g_db_icons[i].srv && (g_db_icons[i].waiting || e.a == GAMEDB_LOADED)) g_db_icons[i].tried = false;
    for (i32 i = 0; i < g_db_cover_count; i++)
        if (!g_db_covers[i].srv && (g_db_covers[i].waiting || e.a == GAMEDB_LOADED)) g_db_covers[i].tried = false;
    for (i32 i = 0; i < g_art_count; i++)
        if (g_art[i].state == ART_DONE && !g_art[i].srv && (g_art[i].waiting || e.a == GAMEDB_LOADED)) {
            g_art[i].state = ART_NEW;
            g_art_worker.dirty = true;
        }
    if (e.a == GAMEDB_LOADED) g_art_worker.dirty = true;
    if (!g_app.config.flat_look && (e.a == GAMEDB_LOADED || e.a == GAMEDB_ART)) art_worker_wake(true);
    art_textures_update(); // with the window hidden too, so the textures are ready when it shows
    ui_wake();
}

// Entering Options again tries the ones that had no icon (a game may be running now).
static void app_icons_retry()
{
    for (i32 i = 0; i < g_app_icon_count; i++)
        if (!g_app_icons[i].srv) g_app_icons[i].tried = false;
}
