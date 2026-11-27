// Open with: video editors a clip opens in the way Explorer's Open with opens it, the editor started with the
// video as its one argument. A clip with an edit sends its playable copy (Clips\, render.cpp) when that is made
// from the edit as it is now, else the original. Blick, VEGAS Pro and CapCut are found where they install, unless
// the player picked the exe in Options; exes the player added open the same way. Each is shown with its exe's own
// icon, and only the ones found are listed. DaVinci Resolve is listed first when it's found, but it takes no file on
// its command line: from the editor it gets the edit's XML instead (ed_open_in), and the Library leaves it out.

struct EditorKind {
    const char    *name;
    const wchar_t *listed; // its name in Windows' list of installed programs, or the start of it before a space
    const wchar_t *exe;    // its exe's name; one * stands for any run of characters
    const wchar_t *dir;    // where it installs, with %variables%; one * in the last folder for a version
};
static const EditorKind EDITOR_KIND[EDITOR_KINDS] = {
    {"Blick", L"Blick Video Editor", L"blick.exe", L"%ProgramFiles%\\BlickVideoEditor"},
    {"VEGAS Pro", L"VEGAS Pro", L"vegas*.exe", L"%ProgramFiles%\\VEGAS\\VEGAS Pro *"},
    {"CapCut", L"CapCut", L"CapCut.exe", L"%LOCALAPPDATA%\\CapCut\\Apps"},
};

struct OpenWith {
    char name[64];
    Path exe;
    i32  kind;  // EDITOR_KIND's, or -1 for one the player added
    bool xml;   // DaVinci Resolve, which takes the editor's XML
    i32  added; // its slot in Config.editors_added
    ID3D11ShaderResourceView *icon;
    bool icon_tried;
};
static OpenWith g_open_with[1 + EDITOR_KINDS + EDITORS_ADDED];
static i32 g_open_with_count;
static bool g_open_with_found;

static bool render_current(const LibItem &it); // render.cpp
static bool editor_exe(Path *out);             // options.cpp: DaVinci Resolve's
static bool path_is_file(const wchar_t *p);    // options.cpp
static Path lib_item_path(const LibItem &it);  // library.cpp
static Path lib_playable_path(const LibItem &it);
static Path lib_edit_path(const LibItem &it);
static void edit_read(const Path &file, EditRecord *e); // editor.cpp
static bool edit_is_default(const EditRecord &e);

static bool name_fits(const wchar_t *name, const wchar_t *pat)
{
    const wchar_t *star = wcschr(pat, L'*');
    if (!star) return !_wcsicmp(name, pat);
    size_t pre = star - pat, post = wcslen(star + 1), n = wcslen(name);
    return n >= pre + post && !_wcsnicmp(name, pat, pre) && !_wcsicmp(name + n - post, star + 1);
}

// The first number in a name and the one after its dot: "VEGAS Pro 22.0" 22000, "vegas220.exe" 220000.
static i64 name_version(const wchar_t *s)
{
    while (*s && !iswdigit(*s)) s++;
    i64 major = wcstol(s, (wchar_t **)&s, 10), minor = *s == L'.' ? wcstol(s + 1, 0, 10) : 0;
    return major * 1000 + minor;
}

// The exe in dir whose name fits pat (the shortest name of those, an uninstaller never).
static bool exe_in(const wchar_t *dir, const wchar_t *pat, Path *out)
{
    wchar_t spec[MAX_PATH];
    swprintf(spec, MAX_PATH, L"%s\\%s", dir, pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t best = 0;
    do {
        size_t n = wcslen(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || wcsstr(fd.cFileName, L"unins") || (best && n >= best)) continue;
        swprintf(out->s, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        best = n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best > 0;
}

// Where it installs, the newest version folder when there are several.
static bool editor_in_dir(const EditorKind &k, Path *out)
{
    wchar_t dir[MAX_PATH];
    if (!ExpandEnvironmentStringsW(k.dir, dir, MAX_PATH) || wcschr(dir, L'%')) return false;
    if (!wcschr(dir, L'*')) return exe_in(dir, k.exe, out);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(dir, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    wchar_t *slash = wcsrchr(dir, L'\\');
    i64 best = -1;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name_version(fd.cFileName) <= best) continue;
        wchar_t sub[MAX_PATH];
        swprintf(sub, MAX_PATH, L"%.*s\\%s", (int)(slash - dir), dir, fd.cFileName);
        if (exe_in(sub, k.exe, out)) best = name_version(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best >= 0;
}

// Windows' list of installed programs: the newest one with the editor's name, its exe from its icon or the
// folder it's in.
static bool editor_listed(const EditorKind &k, Path *out)
{
    static const struct { HKEY root; const wchar_t *path; } places[] = {
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
    };
    size_t ln = wcslen(k.listed);
    i64 best = -1;
    for (i32 p = 0; p < COUNT(places); p++) {
        HKEY key;
        if (RegOpenKeyExW(places[p].root, places[p].path, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        wchar_t sub[256];
        for (DWORD i = 0;; i++) {
            DWORD len = COUNT(sub);
            if (RegEnumKeyExW(key, i, sub, &len, 0, 0, 0, 0) != ERROR_SUCCESS) break;
            wchar_t name[256], icon[MAX_PATH], dir[MAX_PATH];
            DWORD bytes = sizeof name;
            if (RegGetValueW(key, sub, L"DisplayName", RRF_RT_REG_SZ, 0, name, &bytes) != ERROR_SUCCESS) continue;
            if (_wcsnicmp(name, k.listed, ln) || (name[ln] && name[ln] != L' ') || name_version(name) <= best) continue;
            Path exe = {};
            bytes = sizeof icon;
            if (RegGetValueW(key, sub, L"DisplayIcon", RRF_RT_REG_SZ, 0, icon, &bytes) == ERROR_SUCCESS) {
                // "C:\path\file.exe",0 -> C:\path\file.exe
                wchar_t *from = icon[0] == L'"' ? icon + 1 : icon;
                if (wchar_t *q = wcschr(from, L'"')) *q = 0;
                if (wchar_t *comma = wcsrchr(from, L','))
                    if (comma > from + 3 && (iswdigit(comma[1]) || comma[1] == L'-')) *comma = 0;
                const wchar_t *file = wcsrchr(from, L'\\');
                if (file && name_fits(file + 1, k.exe) && path_is_file(from)) swprintf(exe.s, MAX_PATH, L"%s", from);
            }
            bytes = sizeof dir;
            if (!exe.s[0] && RegGetValueW(key, sub, L"InstallLocation", RRF_RT_REG_SZ, 0, dir, &bytes) == ERROR_SUCCESS &&
                dir[0]) {
                size_t n = wcslen(dir);
                if (dir[n - 1] == L'\\') dir[n - 1] = 0;
                exe_in(dir, k.exe, &exe);
            }
            if (!exe.s[0]) continue;
            *out = exe;
            best = name_version(name);
        }
        RegCloseKey(key);
    }
    return best >= 0;
}

// App Paths, for an exe with one name: the path Windows' Run box finds it by.
static bool editor_app_path(const EditorKind &k, Path *out)
{
    if (wcschr(k.exe, L'*')) return false;
    wchar_t key[MAX_PATH];
    swprintf(key, MAX_PATH, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%s", k.exe);
    HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    for (HKEY root : roots) {
        wchar_t v[MAX_PATH];
        DWORD bytes = sizeof v;
        if (RegGetValueW(root, key, 0, RRF_RT_REG_SZ, 0, v, &bytes) != ERROR_SUCCESS) continue;
        wchar_t *from = v[0] == L'"' ? v + 1 : v;
        if (wchar_t *q = wcschr(from, L'"')) *q = 0;
        if (path_is_file(from)) {
            swprintf(out->s, MAX_PATH, L"%s", from);
            return true;
        }
    }
    return false;
}

static bool editor_kind_exe(i32 kind, Path *out)
{
    const Path &set = g_app.config.editors[kind];
    if (set.s[0] && path_is_file(set.s)) {
        *out = set;
        return true;
    }
    const EditorKind &k = EDITOR_KIND[kind];
    return editor_app_path(k, out) || editor_listed(k, out) || editor_in_dir(k, out);
}

// Looks for them all again: at start, and when Options is opened or changes one.
static void open_with_find()
{
    for (i32 i = 0; i < g_open_with_count; i++)
        if (g_open_with[i].icon) g_open_with[i].icon->Release();
    g_open_with_count = 0;
    g_open_with_found = true;
    OpenWith r = {};
    if (editor_exe(&r.exe)) {
        str_set(r.name, "DaVinci Resolve");
        r.kind = -1, r.xml = true;
        g_open_with[g_open_with_count++] = r;
    }
    for (i32 k = 0; k < EDITOR_KINDS; k++) {
        OpenWith o = {};
        if (!editor_kind_exe(k, &o.exe)) continue;
        str_set(o.name, EDITOR_KIND[k].name);
        o.kind = k;
        g_open_with[g_open_with_count++] = o;
    }
    for (i32 a = 0; a < EDITORS_ADDED; a++) {
        const Path &p = g_app.config.editors_added[a];
        if (!p.s[0] || !path_is_file(p.s)) continue;
        OpenWith o = {};
        o.exe = p, o.kind = -1, o.added = a;
        char u[MAX_PATH * 3];
        wide_to_utf8(p.s, u, sizeof u);
        exe_description(u, o.name, sizeof o.name);
        if (!o.name[0]) { // its file name without .exe
            const char *file = strrchr(u, '\\');
            str_set(o.name, file ? file + 1 : u);
            if (char *dot = strrchr(o.name, '.')) *dot = 0;
        }
        g_open_with[g_open_with_count++] = o;
    }
}

static i32 open_with_count()
{
    if (!g_open_with_found) open_with_find();
    return g_open_with_count;
}

static ID3D11ShaderResourceView *open_with_icon(OpenWith &o)
{
    if (!o.icon && !o.icon_tried) {
        o.icon_tried = true;
        o.icon = app_icon_load(o.exe);
    }
    return o.icon;
}

// The file an editor is given for a clip: its playable copy when that has the edit as it is now, else the original.
// *without_edit is set when the clip has an edit the file given doesn't have.
static Path open_with_file(const LibItem &item, bool *without_edit)
{
    *without_edit = false;
    const LibItem *it = &item;
    for (i32 i = 0; i < g_app.library.item_count; i++) // the library's, which knows whether it's rendered now
        if (g_app.library.items[i].key == item.key) { it = &g_app.library.items[i]; break; }
    EditRecord rec;
    edit_read(lib_edit_path(*it), &rec);
    if (it->dir || edit_is_default(rec)) return lib_item_path(*it);
    if (render_current(*it)) return lib_playable_path(*it);
    *without_edit = true;
    return lib_item_path(*it);
}

// Starts editor i with the clip. Says what happened in msg.
static bool open_with(i32 i, const LibItem &item, char *msg, i32 cap)
{
    OpenWith &o = g_open_with[i];
    bool without_edit;
    Path file = open_with_file(item, &without_edit);
    wchar_t arg[MAX_PATH + 2], dir[MAX_PATH];
    swprintf(arg, COUNT(arg), L"\"%s\"", file.s);
    wcscpy_s(dir, o.exe.s);
    if (wchar_t *slash = wcsrchr(dir, L'\\')) *slash = 0;
    char f8[MAX_PATH * 3];
    wide_to_utf8(file.s, f8, sizeof f8);
    if ((INT_PTR)ShellExecuteW(0, L"open", o.exe.s, arg, dir, SW_SHOWNORMAL) <= 32) {
        log_line("open with: %s didn't start (%d)", o.name, (int)GetLastError());
        snprintf(msg, cap, "%s didn't start", o.name);
        return false;
    }
    log_line("open with: %s with %s", o.name, f8);
    if (without_edit) snprintf(msg, cap, "Opened the original in %s: the edited copy isn't made yet", o.name);
    else snprintf(msg, cap, "Opened in %s", o.name);
    return true;
}

// An editor's square button: its icon, or a tile with its first letter while it has none.
static bool open_with_button(i32 i, f32 size)
{
    OpenWith &o = g_open_with[i];
    char id[32];
    snprintf(id, sizeof id, "##openwith%d", i);
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    kit_tip(o.name);
    bool hot = ImGui::IsItemHovered(), held = hot && ImGui::IsItemActive();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 z(a.x + size, a.y + size);
    dl->AddRectFilled(a, z, col32(hot ? COL_HOVER : COL_PANEL2, held ? 0.8f : 1), kit_px(6));
    f32 pad = floorf(size * 0.18f);
    if (ID3D11ShaderResourceView *icon = open_with_icon(o))
        dl->AddImage((ImTextureID)(intptr_t)icon, ImVec2(a.x + pad, a.y + pad), ImVec2(z.x - pad, z.y - pad));
    else {
        char letter[2] = {o.name[0], 0};
        ImVec2 ls = ImGui::CalcTextSize(letter);
        dl->AddText(ImVec2(floorf(a.x + (size - ls.x) / 2), floorf(a.y + (size - ls.y) / 2)), col32(COL_TEXT), letter);
    }
    if (clicked) dev_click("button", o.name);
    return clicked;
}
