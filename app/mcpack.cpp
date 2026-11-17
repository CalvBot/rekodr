// Minecraft's modpacks. javaw.exe is every Minecraft there is, so which modpack a running one is comes from the
// record its launcher keeps by the game's folder (the --gameDir on its command line). What is sent to the art
// service to name the pack is only: the launcher, the pack's platform and its project and version ids, the
// Minecraft version and loader, and the names of the mods (their file names with the versions cut off). Never a
// path, and never an instance's name, which the player chose. Where each launcher keeps its record, from the game's
// folder:
//   Prism, MultiMC   ..\instance.cfg: ManagedPack, ManagedPackType (flame = CurseForge, modrinth), ManagedPackID,
//                    ManagedPackVersionID
//   CurseForge       minecraftinstance.json (in it, or one up): projectID, fileID (0 for a pack imported from a zip)
//   FTB App          ..\instance.json (or in it): id, versionId
//   GDLauncher       ..\instance.json: modpack {platform, project_id, version_id or file_id}
//   ATLauncher       instance.json: launcher {modrinthProject {id}, curseForgeProject {id}} (from its source; not
//                    tried on a real one)
//   Modrinth App     nothing near it (its records are in a database), so its packs go by their mods
// No launcher's account files are ever opened.

struct McClues {
    char launcher[16], platform[16], project[48], version[48], mc[24], loader[12];
    char mods[300][48];
    i32  mod_count;
};

// The value of one argument on a command line: --gameDir X, or --gameDir "X Y". False if it isn't there.
static bool mc_arg(const char *cmd, const char *name, char *out, i32 cap)
{
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = cmd; (p = strstr(p, name)) != 0; p += nl) {
        if (p > cmd && p[-1] != ' ' && p[-1] != '"') continue;
        const char *v = p + nl;
        if (*v != ' ' && *v != '=') continue;
        while (*v == ' ' || *v == '=') v++;
        i32 n = 0;
        if (*v == '"') {
            for (v++; *v && *v != '"' && n < cap - 1;) out[n++] = *v++;
        } else {
            while (*v && *v != ' ' && n < cap - 1) out[n++] = *v++;
        }
        out[n] = 0;
        return n > 0;
    }
    return false;
}

// A mod's name from its file's: "jei-1.20.1-forge-15.2.0.27.jar" is "jei", "[1.12.2]Botania r1.10.jar" "botania".
static void mc_mod_name(const char *file, char *out, i32 cap)
{
    const char *s = file;
    if (*s == '[') { // a version in brackets first
        const char *close = strchr(s, ']');
        if (close) s = close + 1;
    }
    while (*s == ' ' || *s == '-' || *s == '_') s++;
    i32 n = 0;
    for (; s[0] && n < cap - 1; s++) {
        char c = (char)tolower((u8)s[0]), d = (char)tolower((u8)s[1]);
        if (c == '[' || c == '(' || (c == '.' && !_stricmp(s, ".jar"))) break;
        bool sep = c == '-' || c == '_' || c == '+' || c == ' ';
        if (sep && (isdigit((u8)d) || (d == 'm' && tolower((u8)s[2]) == 'c') || (isalpha((u8)d) && isdigit((u8)s[2])) ||
                    d == '[' || d == '(')) break;
        out[n++] = c;
    }
    while (n && (out[n - 1] == '-' || out[n - 1] == '_' || out[n - 1] == ' ' || out[n - 1] == '.')) n--;
    out[n] = 0;
}

// key=value from an .ini-like file
static bool ini_get(const char *text, const char *key, char *out, i32 cap)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *line = text; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : 0) {
        if (strncmp(line, key, kl) || line[kl] != '=') continue;
        const char *v = line + kl + 1;
        i32 n = 0;
        while (v[n] && v[n] != '\r' && v[n] != '\n' && n < cap - 1) { out[n] = v[n]; n++; }
        out[n] = 0;
        return n > 0;
    }
    return false;
}

static char *mc_read(const Path &dir, const wchar_t *file, i64 *len)
{
    *len = 0;
    char *t = read_file(path_join(dir, file).s, len);
    if (t && *len <= 0) { free(t); t = 0; }
    return t;
}

// What the game's folder and the record by it say. False if there is no folder.
static bool mc_read_record(const char *game_dir, McClues *c)
{
    *c = {};
    Path here = {};
    utf8_to_wide(game_dir, here.s, MAX_PATH);
    size_t hl = wcslen(here.s);
    while (hl > 3 && (here.s[hl - 1] == L'\\' || here.s[hl - 1] == L'/')) here.s[--hl] = 0;
    if (!hl || GetFileAttributesW(here.s) == INVALID_FILE_ATTRIBUTES) return false;
    Path up = here;
    wchar_t *cut = wcsrchr(up.s, L'\\'), *cut2 = wcsrchr(up.s, L'/');
    if (cut2 > cut) cut = cut2;
    if (cut) *cut = 0;

    i64 len = 0;
    char *t = 0, v[64];
    if ((t = mc_read(up, L"instance.cfg", &len)) != 0) {
        str_set(c->launcher, "prism");
        if (ini_get(t, "ManagedPack", v, sizeof v) && !_stricmp(v, "true") && ini_get(t, "ManagedPackType", v, sizeof v)) {
            str_set(c->platform, !_stricmp(v, "flame") ? "curseforge" : !_stricmp(v, "modrinth") ? "modrinth" : "");
            ini_get(t, "ManagedPackID", c->project, sizeof c->project);
            ini_get(t, "ManagedPackVersionID", c->version, sizeof c->version);
        }
    } else if ((t = mc_read(here, L"minecraftinstance.json", &len)) != 0 || (t = mc_read(up, L"minecraftinstance.json", &len)) != 0) {
        str_set(c->launcher, "curseforge");
        if (j_get(t, len, "projectID", c->project, sizeof c->project) && strcmp(c->project, "0")) {
            str_set(c->platform, "curseforge");
            j_get(t, len, "fileID", c->version, sizeof c->version);
        } else {
            c->project[0] = 0;
        }
    } else if ((t = mc_read(up, L"instance.json", &len)) != 0 || (t = mc_read(here, L"instance.json", &len)) != 0) {
        if (j_get(t, len, "modpack.project_id", c->project, sizeof c->project)) {
            str_set(c->launcher, "gdlauncher");
            j_get(t, len, "modpack.platform", v, sizeof v);
            str_set(c->platform, !_stricmp(v, "curseforge") ? "curseforge" : !_stricmp(v, "modrinth") ? "modrinth" : "");
            if (!j_get(t, len, "modpack.version_id", c->version, sizeof c->version))
                j_get(t, len, "modpack.file_id", c->version, sizeof c->version);
        } else if (j_get(t, len, "launcher.modrinthProject.id", c->project, sizeof c->project)) {
            str_set(c->launcher, "atlauncher");
            str_set(c->platform, "modrinth");
        } else if (j_get(t, len, "launcher.curseForgeProject.id", c->project, sizeof c->project)) {
            str_set(c->launcher, "atlauncher");
            str_set(c->platform, "curseforge");
        } else if (j_get(t, len, "versionId", c->version, sizeof c->version) && j_get(t, len, "id", c->project, sizeof c->project)) {
            str_set(c->launcher, "ftb");
            str_set(c->platform, "ftb");
        }
    }
    free(t);
    if (!c->platform[0]) c->project[0] = c->version[0] = 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path_join(path_join(here, L"mods"), L"*.jar").s, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char file[160], name[48];
            wide_to_utf8(fd.cFileName, file, sizeof file);
            mc_mod_name(file, name, sizeof name);
            if (strlen(name) >= 2 && c->mod_count < COUNT(c->mods)) str_set(c->mods[c->mod_count++], name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return true;
}

// A string into JSON, quoted; anything that isn't printable is left out.
static void mc_put(char *out, i32 cap, i32 *n, const char *s)
{
    if (*n < cap - 1) out[(*n)++] = '"';
    for (; *s && *n < cap - 3; s++) {
        if ((u8)*s < ' ') continue;
        if (*s == '"' || *s == '\\') out[(*n)++] = '\\';
        out[(*n)++] = *s;
    }
    if (*n < cap - 1) out[(*n)++] = '"';
    out[*n] = 0;
}

// The art service's clues for a running Minecraft, from its command line: {"mc":{...}}. False if it isn't one, or
// its folder can't be found.
static bool mc_clues(const char *cmdline, char *out, i32 cap)
{
    char dir[MAX_PATH * 2], ver[64];
    static char low[32768]; // modded command lines run long
    i32 l = 0;
    for (; cmdline[l] && l < (i32)sizeof low - 1; l++) low[l] = (char)tolower((u8)cmdline[l]);
    low[l] = 0;
    if (!strstr(low, "minecraft") || !mc_arg(cmdline, "--gameDir", dir, sizeof dir)) return false;
    static McClues c; // big; the main thread is the only one asking
    if (!mc_read_record(dir, &c)) return false;
    if (mc_arg(cmdline, "--version", ver, sizeof ver)) {
        i32 n = 0;
        while (n < (i32)sizeof c.mc - 1 && (isdigit((u8)ver[n]) || ver[n] == '.')) { c.mc[n] = ver[n]; n++; }
        c.mc[n] = 0;
    }
    str_set(c.loader, strstr(low, "net.fabricmc") ? "fabric" : strstr(low, "org.quiltmc") ? "quilt"
                      : strstr(low, "neoforge") ? "neoforge" : strstr(low, "forge") || strstr(low, "fml") ? "forge" : "vanilla");
    if (!c.launcher[0]) str_set(c.launcher, "other");

    i32 n = snprintf(out, cap, "{\"mc\":{\"launcher\":");
    mc_put(out, cap, &n, c.launcher);
    n += snprintf(out + n, cap - n, ",\"mc\":");
    mc_put(out, cap, &n, c.mc);
    n += snprintf(out + n, cap - n, ",\"loader\":");
    mc_put(out, cap, &n, c.loader);
    if (c.platform[0]) {
        n += snprintf(out + n, cap - n, ",\"pack\":{\"platform\":");
        mc_put(out, cap, &n, c.platform);
        n += snprintf(out + n, cap - n, ",\"project\":");
        mc_put(out, cap, &n, c.project);
        n += snprintf(out + n, cap - n, ",\"version\":");
        mc_put(out, cap, &n, c.version);
        n += snprintf(out + n, cap - n, "}");
    }
    n += snprintf(out + n, cap - n, ",\"mods\":[");
    for (i32 i = 0; i < c.mod_count && n < cap - 120; i++) {
        if (i) out[n++] = ',';
        mc_put(out, cap, &n, c.mods[i]);
    }
    n += snprintf(out + n, cap - n, "]}}");
    return n < cap - 1;
}
