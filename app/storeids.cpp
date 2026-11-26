// What a store's install says about a game. The service is sent only: the exe's file name, the name of the game's
// own folder and of up to three folders it sits in (never a path, and never a profile's folder or one above it),
// the exe's File description, and the store id its launcher left by it:
//   Steam   <library>\steamapps\appmanifest_<appid>.acf, whose "installdir" is the folder under steamapps\common
//   GOG     goggame-<id>.info in the game's folder
//   Epic    the AppName of the launcher's install manifest (ProgramData\Epic\EpicGamesLauncher\Data\Manifests\*.item)
//           whose InstallLocation holds the exe
// Only a game is asked about: one sorted as a Game, one Discord's list knows, or one a store installed. No
// launcher's account files are ever opened.

// Whether the folder low[s..e) (lower case, in a path with backslashes) could name the player rather than a game:
// a profile folder (x:\users\<name>), one Windows makes in a profile (Desktop, Downloads, OneDrive - <company>...),
// or one named like their Windows account. Such a folder is never sent anywhere.
static bool personal_folder(const char *low, i32 s, i32 e)
{
    i32 len = e - s;
    static const char *const made[] = {"desktop", "downloads", "documents", "pictures", "videos", "music", "users",
                                       "public", "appdata", "local", "locallow", "roaming", "programs",
                                       "saved games", "favorites", "links", "contacts", "searches"};
    for (auto m : made)
        if ((i32)strlen(m) == len && !strncmp(low + s, m, len)) return true;
    if (len >= 8 && !strncmp(low + s, "onedrive", 8)) return true;
    if (s >= 7 && !strncmp(low + s - 7, "\\users\\", 7)) return true; // right in Users: a profile
    wchar_t wuser[128];
    char user[384];
    DWORD n = GetEnvironmentVariableW(L"USERNAME", wuser, COUNT(wuser));
    if (!n || n >= COUNT(wuser)) return false;
    wide_to_utf8(wuser, user, sizeof user);
    return (i32)strlen(user) == len && !_strnicmp(low + s, user, len);
}

// The folder a game is installed in, from its exe's path: the one under steamapps\common if it is there, else the
// exe's own, leaving out the folders engines put exes in ("Binaries\Win64"). False if there is no folder, or the
// folder could name the player (personal_folder).
static bool game_folder(const char *path, char *out, i32 cap, bool *steam)
{
    out[0] = 0;
    *steam = false;
    char low[MAX_PATH * 3];
    i32 n = 0;
    for (; path[n] && n < (i32)sizeof low - 1; n++) low[n] = (char)tolower((u8)(path[n] == '/' ? '\\' : path[n]));
    low[n] = 0;
    const char *common = strstr(low, "\\steamapps\\common\\");
    if (common) {
        const char *s = path + (common - low) + 18, *e = s;
        while (*e && *e != '\\' && *e != '/') e++;
        if (!*e || e == s) return false; // the exe is right in common: no game's folder
        snprintf(out, cap, "%.*s", (i32)(e - s), s);
        *steam = true;
        return true;
    }
    static const char *const generic[] = {"bin", "binaries", "win64", "win32", "x64", "x86", "x86_64", "game",
                                          "retail", "release", "shipping", "client", "windows", "windowsnoeditor"};
    i32 end = n;
    while (end > 0 && low[end - 1] != '\\') end--; // past the exe's own name
    for (i32 depth = 0; depth < 4 && end > 0; depth++) {
        i32 e = end - 1, s = e; // the folder is low[s..e)
        while (s > 0 && low[s - 1] != '\\') s--;
        if (s == 0 || e <= s || low[e - 1] == ':') return false; // up to the drive
        bool skip = false;
        for (auto g : generic) skip |= (i32)strlen(g) == e - s && !strncmp(low + s, g, e - s);
        if (!skip) {
            if (personal_folder(low, s, e)) return false;
            snprintf(out, cap, "%.*s", e - s, path + s);
            return true;
        }
        end = s;
    }
    return false;
}

// The Steam app a folder under steamapps\common belongs to, from its library's appmanifests. 0 if none says so.
static u32 steam_app_for(const char *path, const char *folder)
{
    char low[MAX_PATH * 3];
    i32 n = 0;
    for (; path[n] && n < (i32)sizeof low - 1; n++) low[n] = (char)tolower((u8)(path[n] == '/' ? '\\' : path[n]));
    low[n] = 0;
    const char *at = strstr(low, "\\steamapps\\common\\");
    if (!at) return 0;
    char lib[MAX_PATH * 3];
    snprintf(lib, sizeof lib, "%.*s\\steamapps", (i32)(at - low), path);
    Path dir = {};
    utf8_to_wide(lib, dir.s, MAX_PATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path_join(dir, L"appmanifest_*.acf").s, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    u32 found = 0;
    do {
        u32 id = (u32)wcstoul(fd.cFileName + 12, 0, 10);
        if (!id || fd.nFileSizeLow > 256 * 1024) continue;
        i64 len = 0;
        char *t = read_file(path_join(dir, fd.cFileName).s, &len);
        if (!t) continue;
        // "installdir"		"Hollow Knight"
        const char *k = len > 0 ? strstr(t, "\"installdir\"") : 0;
        if (k) {
            const char *q = strchr(k + 12, '"'), *e = q ? strchr(q + 1, '"') : 0;
            if (e && (size_t)(e - q - 1) == strlen(folder) && !_strnicmp(q + 1, folder, e - q - 1)) found = id;
        }
        free(t);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// The GOG game installed in the folder an exe is in or above (up to three), from its goggame-<id>.info. 0 if none.
static u64 gog_game_for(const char *path)
{
    Path dir = {};
    utf8_to_wide(path, dir.s, MAX_PATH);
    for (i32 depth = 0; depth < 4; depth++) {
        wchar_t *cut = wcsrchr(dir.s, L'\\'), *cut2 = wcsrchr(dir.s, L'/');
        if (cut2 > cut) cut = cut2;
        if (!cut || cut - dir.s < 3) return 0;
        *cut = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(path_join(dir, L"goggame-*.info").s, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            u64 id = wcstoull(fd.cFileName + 8, 0, 10);
            FindClose(h);
            if (id) return id;
        }
    }
    return 0;
}

// The Epic game whose install folder holds the exe, from the launcher's install manifests: its AppName, into out.
// False if none.
static bool epic_app_for(const char *path, char *out, i32 cap)
{
    out[0] = 0;
    wchar_t data[MAX_PATH];
    DWORD dn = GetEnvironmentVariableW(L"ProgramData", data, COUNT(data));
    if (!dn || dn >= COUNT(data)) return false;
    Path dir = {};
    swprintf(dir.s, MAX_PATH, L"%ls\\Epic\\EpicGamesLauncher\\Data\\Manifests", data);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path_join(dir, L"*.item").s, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.nFileSizeHigh || fd.nFileSizeLow > 1024 * 1024) continue;
        i64 len = 0;
        char *t = read_file(path_join(dir, fd.cFileName).s, &len);
        if (!t) continue;
        char at[MAX_PATH * 3], app[128];
        if (len > 0 && j_get(t, len, "InstallLocation", at, sizeof at) && j_get(t, len, "AppName", app, sizeof app) && at[0]) {
            size_t n = strlen(at);
            while (n && (at[n - 1] == '\\' || at[n - 1] == '/')) n--;
            bool in = n > 2 && strlen(path) > n && (path[n] == '\\' || path[n] == '/');
            for (size_t i = 0; in && i < n; i++) {
                char a = (char)tolower((u8)(at[i] == '/' ? '\\' : at[i])), b = (char)tolower((u8)(path[i] == '/' ? '\\' : path[i]));
                in = a == b;
            }
            if (in && app[0]) snprintf(out, cap, "%s", app);
        }
        free(t);
    } while (!out[0] && FindNextFileW(h, &fd));
    FindClose(h);
    return out[0] != 0;
}

// The names of up to three folders an exe sits in, nearest last, as a JSON array: ["Games","Hollow Knight"].
// None that could name the player (personal_folder), nor any above one. False if there are none.
static bool exe_dirs(const char *path, char *out, i32 cap)
{
    char low[MAX_PATH * 3];
    i32 n = 0;
    for (; path[n] && n < (i32)sizeof low - 1; n++) low[n] = (char)tolower((u8)(path[n] == '/' ? '\\' : path[n]));
    low[n] = 0;
    i32 from[3], to[3], count = 0, end = n;
    while (end > 0 && low[end - 1] != '\\') end--; // past the exe's own name
    while (count < 3 && end > 0) {
        i32 e = end - 1, s = e; // the folder is low[s..e)
        while (s > 0 && low[s - 1] != '\\') s--;
        if (e <= s || low[e - 1] == ':' || personal_folder(low, s, e)) break;
        from[count] = s;
        to[count++] = e;
        end = s;
    }
    if (!count) return false;
    i32 w = snprintf(out, cap, "[");
    for (i32 i = count - 1; i >= 0; i--) {
        char one[MAX_PATH];
        snprintf(one, sizeof one, "%.*s", to[i] - from[i], path + from[i]);
        if (i != count - 1 && w < cap - 1) out[w++] = ',';
        mc_put(out, cap, &w, one);
    }
    snprintf(out + w, cap - w, "]");
    return w < cap - 2;
}

// The service's clues for an exe: {"exe","folder","dirs","name","steam","gog","epic"}, then `more` (more fields, each
// with its comma first). False if it isn't to be asked about: not a game (game false, and no store installed it),
// or no folder to go by.
static bool exe_clues(const char *exe, const char *path, const char *name, bool game, const char *more, char *out,
                      i32 cap)
{
    char folder[MAX_PATH], dirs[MAX_PATH * 3];
    bool in_steam;
    if (!game_folder(path, folder, sizeof folder, &in_steam)) return false;
    u32 steam = in_steam ? steam_app_for(path, folder) : 0;
    u64 gog = steam ? 0 : gog_game_for(path);
    char epic[128] = "";
    if (!steam && !gog) epic_app_for(path, epic, sizeof epic);
    if (!game && !in_steam && !gog && !epic[0]) return false;
    i32 n = snprintf(out, cap, "{\"exe\":");
    mc_put(out, cap, &n, exe);
    n += snprintf(out + n, cap - n, ",\"folder\":");
    mc_put(out, cap, &n, folder);
    if (exe_dirs(path, dirs, sizeof dirs)) n += snprintf(out + n, cap - n, ",\"dirs\":%s", dirs);
    if (name && name[0]) {
        n += snprintf(out + n, cap - n, ",\"name\":");
        mc_put(out, cap, &n, name);
    }
    if (steam) n += snprintf(out + n, cap - n, ",\"steam\":%u", steam);
    if (gog) n += snprintf(out + n, cap - n, ",\"gog\":\"%llu\"", (unsigned long long)gog);
    if (epic[0]) {
        n += snprintf(out + n, cap - n, ",\"epic\":");
        mc_put(out, cap, &n, epic);
    }
    if (n < cap) n += snprintf(out + n, cap - n, "%s}", more ? more : "");
    return n > 0 && n < cap - 1;
}

// ---------------------------------------------------------------------------------------------------------------
// Store game folders: where Steam, the Xbox app and the Epic launcher install games, added as Game folder rules at
// each start (store_folders_update). Each is added once: Config.store_seen keeps the ones added, so a rule the
// player removed or gave another category stays that way.
//   Steam   each library's steamapps\common (Steam's libraryfolders.vdf), and the folder of each Steam app in it
//           that isn't a game (STEAM_NOT_GAMES) as an App
//   Xbox    the folders each fixed drive's .GamingRoot names (XboxGames)
//   Epic    the InstallLocation of each game the launcher installed (its .item manifests), not Unreal Engine's
//   GOG     the path of each game GOG Galaxy or a GOG installer put in (HKLM SOFTWARE\GOG.com\Games\<id>)
//   EA      the Install Dir of each game the EA app or Origin put in (HKLM SOFTWARE\EA Games, Electronic Arts and
//           Respawn, a key per game)
//   Battle.net  the InstallLocation of each game whose uninstaller is Battle.net's agent (Windows' Uninstall keys),
//           not Battle.net's own
//   Riot    each game folder in the Riot Client's ProgramData\Riot Games\RiotClientInstalls.json ("associated_client")

// Steam apps that install under steamapps\common like a game but aren't one. Some run all day beside games.
struct SteamNotGame {
    u32         id;
    const char *name;
};
static const SteamNotGame STEAM_NOT_GAMES[] = {
    {431960, "Wallpaper Engine"},
    {993090, "Lossless Scaling"},
    {250820, "SteamVR"},
    {1176510, "SteamVR Device Monitor"},
    {228980, "Steamworks Common Redistributables"},
    {629520, "Soundpad"},
    {3419430, "Bongo Cat"},
    {3301060, "Desktop Mate"},
    {227260, "DisplayFusion"},
    {607380, "Fences 3"},
    {3165690, "Fences 6"},
    {2296360, "Groupy 2"},
    {1694750, "Start11 v2"},
    {2294630, "WindowBlinds 11"},
    {1325860, "VTube Studio"},
    {1068820, "OVR Toolkit"},
    {908520, "fpsVR"},
    {382110, "Virtual Desktop Classic"},
    {1905180, "OBS Studio"},
    {365670, "Blender"},
    {431730, "Aseprite"},
    {1840, "Source Filmmaker"},
    {223850, "3DMark"},
};

// Steam's folder, from the registry. False if Steam isn't installed.
static bool steam_root(Path *out)
{
    DWORD bytes = sizeof out->s;
    out->s[0] = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, 0, out->s, &bytes) != ERROR_SUCCESS || !out->s[0])
        return false;
    for (wchar_t *c = out->s; *c; c++)
        if (*c == '/') *c = '\\';
    return true;
}

// The value after the first `"key"` in a Steam .vdf or .acf text, its \\ unescaped, into out. False if none.
static bool vdf_value(const char *text, const char *key, char *out, i32 cap, const char **after = 0)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), '"');
    if (!p) return false;
    i32 n = 0;
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) p++;
        if (n < cap - 1) out[n++] = *p;
    }
    out[n] = 0;
    if (after) *after = *p ? p + 1 : p;
    return true;
}

// The libraries Steam's libraryfolders.vdf lists: each "path". Returns how many went into out.
static i32 steam_libraries(const char *vdf, char (*out)[MAX_PATH * 2], i32 max)
{
    i32 n = 0;
    for (const char *p = vdf; n < max && vdf_value(p, "path", out[n], MAX_PATH * 2, &p);)
        if (out[n][0]) n++;
    return n;
}

// The folders a drive's .GamingRoot names, relative to the drive's root: "RGBX", a u32 count, then that many
// UTF-16 names, each ending in a 0. Returns how many went into out (UTF-8).
static i32 gaming_root_folders(const u8 *b, i64 len, char (*out)[MAX_PATH * 2], i32 max)
{
    if (len < 8 || memcmp(b, "RGBX", 4)) return 0;
    u32 count;
    memcpy(&count, b + 4, 4);
    i32 n = 0;
    i64 at = 8;
    for (u32 i = 0; i < count && n < max && at + 2 <= len; i++) {
        wchar_t name[MAX_PATH];
        i32 w = 0;
        for (; at + 2 <= len; at += 2) {
            wchar_t c = (wchar_t)(b[at] | (b[at + 1] << 8));
            if (!c) break;
            if (w < MAX_PATH - 1) name[w++] = c;
        }
        at += 2; // the 0
        name[w] = 0;
        if (w) wide_to_utf8(name, out[n++], MAX_PATH * 2);
    }
    return n;
}

// A game's folder from an Epic launcher .item manifest: its InstallLocation, when its AppCategories has "games"
// (the launcher also installs Unreal Engine and its plugins). False otherwise.
static bool epic_item_folder(const char *text, char *out, i32 cap)
{
    out[0] = 0;
    const char *cats = strstr(text, "\"AppCategories\"");
    const char *open = cats ? strchr(cats, '[') : 0, *close = open ? strchr(open, ']') : 0;
    if (!close) return false;
    bool game = false;
    for (const char *p = open; p && p < close && !game; p = strstr(p + 1, "\"games\"")) game = !strncmp(p, "\"games\"", 7);
    return game && vdf_value(text, "InstallLocation", out, cap) && out[0];
}

struct StoreFolder {
    char        path[MAX_PATH * 2]; // UTF-8, backslashes, no \ at the end
    AppCategory category;
};

static void store_folder_add(StoreFolder *out, i32 *n, i32 max, const char *path, AppCategory cat)
{
    if (*n >= max || !path[0]) return;
    StoreFolder *f = &out[(*n)++];
    str_set(f->path, path);
    for (char *c = f->path; *c; c++)
        if (*c == '/') *c = '\\';
    size_t len = strlen(f->path);
    while (len > 3 && f->path[len - 1] == '\\') f->path[--len] = 0;
    f->category = cat;
}

static bool dir_exists_utf8(const char *path)
{
    wchar_t w[MAX_PATH];
    utf8_to_wide(path, w, MAX_PATH);
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// The folder of each game under `key`, a key per game with its install folder in `value`, in one view of the
// registry (KEY_WOW64_32KEY or KEY_WOW64_64KEY). Ones that aren't there any more are left out.
static void reg_game_folders(HKEY root, const wchar_t *key, const wchar_t *value, REGSAM view, StoreFolder *out, i32 *n,
                             i32 max)
{
    HKEY games;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &games) != ERROR_SUCCESS) return;
    wchar_t id[256];
    for (DWORD i = 0, len = COUNT(id); RegEnumKeyExW(games, i, id, &len, 0, 0, 0, 0) == ERROR_SUCCESS; i++, len = COUNT(id)) {
        wchar_t path[MAX_PATH];
        DWORD bytes = sizeof path;
        if (RegGetValueW(games, id, value, RRF_RT_REG_SZ, 0, path, &bytes) != ERROR_SUCCESS || !path[0]) continue;
        char at[MAX_PATH * 2];
        wide_to_utf8(path, at, sizeof at);
        if (dir_exists_utf8(at)) store_folder_add(out, n, max, at, CAT_GAME);
    }
    RegCloseKey(games);
}

// Whether an Uninstall key's UninstallString is Battle.net's agent removing a game:
//   "C:\ProgramData\Battle.net\Agent\Blizzard Uninstaller.exe" --lang=enUS --uid=prometheus --displayname="Overwatch"
// Battle.net itself is uninstalled the same way, with --uid=battle.net.
static bool battlenet_game_uninstall(const wchar_t *s)
{
    return wcsstr(s, L"\\Battle.net\\Agent\\") && wcsstr(s, L"--uid=") && !wcsstr(s, L"--uid=battle.net");
}

// The folder of each game Battle.net installed, from the Uninstall keys under `key` in one view of the registry
static void battlenet_game_folders(HKEY root, const wchar_t *key, REGSAM view, StoreFolder *out, i32 *n, i32 max)
{
    HKEY apps;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &apps) != ERROR_SUCCESS) return;
    wchar_t id[256];
    for (DWORD i = 0, len = COUNT(id); RegEnumKeyExW(apps, i, id, &len, 0, 0, 0, 0) == ERROR_SUCCESS; i++, len = COUNT(id)) {
        wchar_t un[1024], path[MAX_PATH];
        DWORD ub = sizeof un, pb = sizeof path;
        if (RegGetValueW(apps, id, L"UninstallString", RRF_RT_REG_SZ, 0, un, &ub) != ERROR_SUCCESS || !battlenet_game_uninstall(un))
            continue;
        if (RegGetValueW(apps, id, L"InstallLocation", RRF_RT_REG_SZ, 0, path, &pb) != ERROR_SUCCESS || !path[0]) continue;
        char at[MAX_PATH * 2];
        wide_to_utf8(path, at, sizeof at);
        if (dir_exists_utf8(at)) store_folder_add(out, n, max, at, CAT_GAME);
    }
    RegCloseKey(apps);
}

// The game folders in a RiotClientInstalls.json: the keys of its "associated_client", each a game's folder with the
// Riot Client that runs it as the value ({"E:/Riot Games/League of Legends/": "C:/Riot Games/Riot Client/..."}).
// Returns how many went into out.
static i32 riot_game_folders(const char *text, i64 len, char (*out)[MAX_PATH * 2], i32 max)
{
    Json j = {text, text + len};
    if (!j_find(&j, "associated_client") || !j_eat(&j, '{')) return 0;
    i32 n = 0;
    while (j.p < j.end && !j_eat(&j, '}')) {
        char dir[MAX_PATH * 2];
        if (!j_string(&j, dir, sizeof dir)) break;
        j_eat(&j, ':');
        j_skip(&j);
        j_eat(&j, ',');
        if (dir[0] && n < max) str_set(out[n++], dir);
    }
    return n;
}

// The store game folders on this PC. Returns how many went into out.
static i32 store_folders_find(StoreFolder *out, i32 max)
{
    i32 n = 0;
    Path root;
    if (steam_root(&root)) {
        i64 len = 0;
        char *vdf = read_file(path_join(root, L"steamapps\\libraryfolders.vdf").s, &len);
        static char libs[16][MAX_PATH * 2];
        i32 lib_count = vdf ? steam_libraries(vdf, libs, COUNT(libs)) : 0;
        free(vdf);
        for (i32 l = 0; l < lib_count; l++) {
            char common[MAX_PATH * 2];
            snprintf(common, sizeof common, "%s\\steamapps\\common", libs[l]);
            if (!dir_exists_utf8(common)) continue;
            store_folder_add(out, &n, max, common, CAT_GAME);
            Path lib = {};
            utf8_to_wide(libs[l], lib.s, MAX_PATH);
            for (const SteamNotGame &s : STEAM_NOT_GAMES) {
                wchar_t acf[MAX_PATH];
                swprintf(acf, MAX_PATH, L"%s\\steamapps\\appmanifest_%u.acf", lib.s, s.id);
                char *t = read_file(acf, &len);
                char dir[MAX_PATH];
                if (t && vdf_value(t, "installdir", dir, sizeof dir) && dir[0]) {
                    char app[MAX_PATH * 2];
                    snprintf(app, sizeof app, "%s\\%s", common, dir);
                    store_folder_add(out, &n, max, app, CAT_APP);
                }
                free(t);
            }
        }
    }
    DWORD drives = GetLogicalDrives();
    for (i32 d = 0; d < 26; d++) {
        wchar_t drive[4] = {(wchar_t)(L'A' + d), L':', L'\\', 0};
        if (!(drives & (1u << d)) || GetDriveTypeW(drive) != DRIVE_FIXED) continue;
        Path file = {};
        swprintf(file.s, MAX_PATH, L"%s.GamingRoot", drive);
        i64 len = 0;
        char *b = read_file(file.s, &len);
        char names[4][MAX_PATH * 2];
        i32 count = b ? gaming_root_folders((const u8 *)b, len, names, COUNT(names)) : 0;
        free(b);
        for (i32 i = 0; i < count; i++) {
            char dir[MAX_PATH * 2];
            snprintf(dir, sizeof dir, "%c:\\%s", 'A' + d, names[i]);
            if (dir_exists_utf8(dir)) store_folder_add(out, &n, max, dir, CAT_GAME);
        }
    }
    wchar_t data[MAX_PATH];
    DWORD dn = GetEnvironmentVariableW(L"ProgramData", data, COUNT(data));
    if (dn && dn < COUNT(data)) {
        Path dir = {};
        swprintf(dir.s, MAX_PATH, L"%ls\\Epic\\EpicGamesLauncher\\Data\\Manifests", data);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(path_join(dir, L"*.item").s, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.nFileSizeHigh || fd.nFileSizeLow > 1024 * 1024) continue;
                char *t = read_file(path_join(dir, fd.cFileName).s, 0);
                char at[MAX_PATH * 2];
                if (t && epic_item_folder(t, at, sizeof at) && dir_exists_utf8(at)) store_folder_add(out, &n, max, at, CAT_GAME);
                free(t);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    reg_game_folders(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games", L"path", KEY_WOW64_32KEY, out, &n, max);
    static const wchar_t *const EA_KEYS[] = {L"SOFTWARE\\EA Games", L"SOFTWARE\\Electronic Arts", L"SOFTWARE\\Respawn"};
    static const REGSAM VIEWS[] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    for (const wchar_t *k : EA_KEYS)
        for (REGSAM view : VIEWS)
            reg_game_folders(HKEY_LOCAL_MACHINE, k, L"Install Dir", view, out, &n, max);
    for (REGSAM view : VIEWS)
        battlenet_game_folders(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", view, out, &n,
                               max);
    if (dn && dn < COUNT(data)) {
        Path file = {};
        swprintf(file.s, MAX_PATH, L"%ls\\Riot Games\\RiotClientInstalls.json", data);
        i64 len = 0;
        char *t = read_file(file.s, &len);
        static char riot[16][MAX_PATH * 2];
        i32 count = t ? riot_game_folders(t, len, riot, COUNT(riot)) : 0;
        free(t);
        for (i32 i = 0; i < count; i++)
            if (dir_exists_utf8(riot[i])) store_folder_add(out, &n, max, riot[i], CAT_GAME);
    }
    return n;
}

// Adds each folder found as a folder rule, unless it is one already or was added before (Config.store_seen).
// True if c changed.
static bool store_folders_merge(Config *c, const StoreFolder *found, i32 n)
{
    bool changed = false;
    for (i32 i = 0; i < n; i++) {
        const char *p = found[i].path;
        bool seen = false;
        for (i32 s = 0; s < c->store_seen_count && !seen; s++) seen = !_stricmp(c->store_seen[s], p);
        if (seen || c->store_seen_count == COUNT(c->store_seen)) continue;
        bool rule = false;
        for (i32 r = 0; r < c->folder_count && !rule; r++) rule = !_stricmp(c->folders[r].path, p);
        if (!rule) {
            if (c->folder_count == COUNT(c->folders)) continue;
            FolderRule *r = &c->folders[c->folder_count++];
            str_set(r->path, p);
            r->category = found[i].category;
        }
        str_set(c->store_seen[c->store_seen_count++], p);
        changed = true;
    }
    return changed;
}

// At each start: the store game folders this PC has that aren't rules yet. True if c changed.
static bool store_folders_update(Config *c)
{
    static StoreFolder found[FOLDER_MAX];
    i32 n = store_folders_find(found, COUNT(found));
    bool changed = store_folders_merge(c, found, n);
    if (changed) log_line("store folders: %d found, rules now %d", n, c->folder_count);
    return changed;
}
