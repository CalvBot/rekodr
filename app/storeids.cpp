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
