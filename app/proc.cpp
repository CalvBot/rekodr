// Child processes (ffmpeg, ffprobe) and worker threads.
//
// Every child goes into one job object that kills it when Rekodr exits for any reason, so nothing keeps
// recording unsupervised. Children start without a console window. Any of stdin, stdout and stderr can be a pipe;
// the rest go to NUL. PROC_MERGE sends stderr down the stdout pipe, so one reader sees all output and can't
// deadlock on the other pipe filling up.

enum ProcFlags : u32 {
    PROC_STDIN  = 1,
    PROC_STDOUT = 2,
    PROC_STDERR = 4,
    PROC_MERGE  = 8, // stderr into the stdout pipe (implies PROC_STDOUT)
};

struct Proc {
    HANDLE process;
    DWORD  pid;
    HANDLE in;  // write end of the child's stdin, or 0
    HANDLE out; // read end of the child's stdout, or 0
    HANDLE err; // read end of the child's stderr, or 0
};

static HANDLE g_job;

static void proc_init()
{
    g_job = CreateJobObjectW(0, 0);
    CHECK(g_job, "CreateJobObject failed (%d)", (int)GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info, sizeof info);
}

// Appends one argument to a command line, quoted the way CommandLineToArgvW (and ffmpeg) reads it back.
static void cmd_arg(wchar_t *cmd, i32 cap, const wchar_t *arg)
{
    size_t n = wcslen(cmd);
    auto put = [&](wchar_t c) { if (n + 1 < (size_t)cap) { cmd[n++] = c; cmd[n] = 0; } };
    if (n) put(L' ');
    bool quote = !*arg || wcspbrk(arg, L" \t\"");
    if (!quote) {
        for (const wchar_t *c = arg; *c; c++) put(*c);
        return;
    }
    put(L'"');
    for (const wchar_t *c = arg;; c++) {
        i32 slashes = 0;
        while (*c == L'\\') { slashes++; c++; }
        if (!*c) { for (i32 i = 0; i < slashes * 2; i++) put(L'\\'); break; }
        if (*c == L'"') { for (i32 i = 0; i < slashes * 2 + 1; i++) put(L'\\'); }
        else { for (i32 i = 0; i < slashes; i++) put(L'\\'); }
        put(*c);
    }
    put(L'"');
}

// The UTF-8 version, for arguments built with snprintf.
static void cmd_arg8(wchar_t *cmd, i32 cap, const char *arg)
{
    wchar_t w[2048];
    utf8_to_wide(arg, w, COUNT(w));
    cmd_arg(cmd, cap, w);
}

// Starts `exe` with the command line `args` (arguments only; the exe is put first). Returns false and logs if it
// couldn't start.
static bool proc_start(const wchar_t *exe, const wchar_t *args, u32 flags, Proc *p)
{
    *p = {};
    if (flags & PROC_MERGE) flags |= PROC_STDOUT;
    SECURITY_ATTRIBUTES sa = {sizeof sa, 0, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, 0);
    HANDLE in_r = nul, out_w = nul, err_w = nul;
    if (flags & PROC_STDIN) {
        CreatePipe(&in_r, &p->in, &sa, 1 << 20);
        SetHandleInformation(p->in, HANDLE_FLAG_INHERIT, 0);
    }
    if (flags & PROC_STDOUT) {
        CreatePipe(&p->out, &out_w, &sa, 1 << 16);
        SetHandleInformation(p->out, HANDLE_FLAG_INHERIT, 0);
    }
    if (flags & PROC_MERGE) {
        err_w = out_w;
    } else if (flags & PROC_STDERR) {
        CreatePipe(&p->err, &err_w, &sa, 1 << 16);
        SetHandleInformation(p->err, HANDLE_FLAG_INHERIT, 0);
    }

    static wchar_t cmd[32768];
    static SRWLOCK cmd_lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&cmd_lock);
    cmd[0] = 0;
    cmd_arg(cmd, COUNT(cmd), exe);
    if (args && *args) { wcsncat_s(cmd, COUNT(cmd), L" ", _TRUNCATE); wcsncat_s(cmd, COUNT(cmd), args, _TRUNCATE); }

    // Only the three std handles are inherited, so two children started at once can't hold each other's pipes.
    HANDLE inherit[3];
    i32 inherit_count = 0;
    HANDLE std3[3] = {in_r, out_w, err_w};
    for (HANDLE h : std3) {
        bool seen = false;
        for (i32 i = 0; i < inherit_count; i++) seen |= inherit[i] == h;
        if (!seen) inherit[inherit_count++] = h;
    }
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(0, 1, 0, &attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attr_size);
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, inherit_count * sizeof(HANDLE), 0, 0);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = err_w;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi = {};
    // Started suspended and put in the job before it runs, so it can't outlive Rekodr even briefly.
    BOOL ok = CreateProcessW(0, cmd, 0, 0, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                             0, 0, &si.StartupInfo, &pi);
    DWORD error = GetLastError();
    ReleaseSRWLockExclusive(&cmd_lock);
    DeleteProcThreadAttributeList(attrs);
    HeapFree(GetProcessHeap(), 0, attrs);

    if (in_r != nul) CloseHandle(in_r);
    if (out_w != nul) CloseHandle(out_w);
    if (err_w != nul && err_w != out_w) CloseHandle(err_w);
    CloseHandle(nul);

    if (!ok) {
        char exe8[MAX_PATH * 3];
        wide_to_utf8(exe, exe8, sizeof exe8);
        log_line("couldn't start %s (%d)", exe8, (int)error);
        if (p->in) CloseHandle(p->in);
        if (p->out) CloseHandle(p->out);
        if (p->err) CloseHandle(p->err);
        *p = {};
        return false;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
    p->pid = pi.dwProcessId;
    return true;
}

// Closes the child's stdin, which tells ffmpeg reading from a pipe that the input has ended.
static void proc_close_stdin(Proc *p)
{
    if (p->in) { CloseHandle(p->in); p->in = 0; }
}

// Waits up to `ms` for the child to exit. Returns its exit code, or -1 if it's still running.
static i32 proc_wait(Proc *p, u32 ms)
{
    if (!p->process) return -1;
    if (WaitForSingleObject(p->process, ms) != WAIT_OBJECT_0) return -1;
    DWORD code = 0;
    GetExitCodeProcess(p->process, &code);
    return (i32)code;
}

static void proc_kill(Proc *p)
{
    if (p->process) TerminateProcess(p->process, 1);
}

// Closes every handle. The child keeps running if it hasn't exited (the job still ends it when Rekodr exits).
static void proc_close(Proc *p)
{
    proc_close_stdin(p);
    if (p->out) CloseHandle(p->out);
    if (p->err) CloseHandle(p->err);
    if (p->process) CloseHandle(p->process);
    *p = {};
}

// Reads one line (without the line ending) from a pipe, a byte at a time so nothing past the line is consumed.
// ffmpeg ends progress lines with \r, so \r also ends a line. Returns false at the end of the output.
static bool pipe_read_line(HANDLE pipe, char *line, i32 cap)
{
    i32 n = 0;
    for (;;) {
        char c;
        DWORD got = 0;
        if (!ReadFile(pipe, &c, 1, &got, 0) || got == 0) {
            line[n] = 0;
            return n > 0;
        }
        if (c == '\n' || c == '\r') {
            if (n == 0) continue; // the \n after a \r, or a blank line
            line[n] = 0;
            return true;
        }
        if (n + 1 < cap) line[n++] = c;
    }
}

// Runs a child to the end and returns its exit code (-1 if it couldn't start or took longer than `timeout_ms`, in
// which case it's killed). Its stdout and stderr together go into `output` (cut to fit) if that isn't null.
static i32 proc_run(const wchar_t *exe, const wchar_t *args, u32 timeout_ms, char *output, i32 output_cap)
{
    if (output && output_cap) output[0] = 0;
    Proc p;
    if (!proc_start(exe, args, output ? PROC_MERGE : 0, &p)) return -1;
    i32 n = 0;
    u64 deadline = GetTickCount64() + timeout_ms;
    if (output) {
        // Read until the pipe ends; a reader thread isn't needed because the timeout is checked between reads
        // and a stuck child is killed, which ends the pipe.
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(p.out, 0, 0, 0, &avail, 0)) break; // ended
            if (avail == 0) {
                if (GetTickCount64() > deadline) { proc_kill(&p); break; }
                if (proc_wait(&p, 0) >= 0) {
                    if (!PeekNamedPipe(p.out, 0, 0, 0, &avail, 0) || avail == 0) break;
                } else {
                    Sleep(5);
                    continue;
                }
            }
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(p.out, buf, avail < sizeof buf ? avail : sizeof buf, &got, 0) || got == 0) break;
            i32 keep = (i32)got < output_cap - 1 - n ? (i32)got : output_cap - 1 - n;
            if (keep > 0) { memcpy(output + n, buf, keep); n += keep; output[n] = 0; }
        }
    }
    u64 now = GetTickCount64();
    i32 code = proc_wait(&p, now < deadline ? (u32)(deadline - now) : 0);
    if (code < 0) proc_kill(&p);
    proc_close(&p);
    return code;
}

// ---------------------------------------------------------------------------------------------------------------
// ffmpeg

// Where Rekodr keeps the ffmpeg it fetches for itself (ffget.cpp): local_dir\ffmpeg. Worked out from the
// environment, as paths_init does, so worker threads can call it without the app's paths.
static Path ffget_dir()
{
    Path p = {};
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"REKODR_HOME", home, MAX_PATH) && home[0]) {
        swprintf(p.s, MAX_PATH, L"%s\\local\\ffmpeg", home);
    } else {
        wchar_t *local = 0;
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, 0, &local);
        swprintf(p.s, MAX_PATH, L"%s\\" APP_NAME L"\\ffmpeg", local ? local : L".");
        CoTaskMemFree(local);
    }
    return p;
}

static Path ffget_exe() { return path_join(ffget_dir(), L"ffmpeg-8.1.1-full_build-shared\\bin\\ffmpeg.exe"); }

// Set while Rekodr's own copy is being uninstalled (ffget_uninstall): ffmpeg_find passes over it.
static volatile LONG g_ffget_hidden;

// Where winget links the ffmpeg it installs.
static Path ffmpeg_winget()
{
    Path p = {};
    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
        swprintf(p.s, MAX_PATH, L"%s\\Microsoft\\WinGet\\Links\\ffmpeg.exe", local);
    return p;
}

// Finds ffmpeg.exe: the configured path, then PATH, then winget's links folder, then the one Rekodr fetched.
// ffprobe is expected next to it.
static bool ffmpeg_find(const Path &configured, Path *out)
{
    *out = {};
    if (configured.s[0]) {
        if (GetFileAttributesW(configured.s) == INVALID_FILE_ATTRIBUTES) return false;
        if (g_ffget_hidden && !_wcsicmp(configured.s, ffget_exe().s)) return false;
        *out = configured;
        return true;
    }
    if (SearchPathW(0, L"ffmpeg.exe", 0, MAX_PATH, out->s, 0)) return true;
    *out = ffmpeg_winget();
    if (out->s[0] && GetFileAttributesW(out->s) != INVALID_FILE_ATTRIBUTES) return true;
    *out = ffget_exe();
    if (!g_ffget_hidden && GetFileAttributesW(out->s) != INVALID_FILE_ATTRIBUTES) return true;
    *out = {};
    return false;
}

static Path ffprobe_for(const Path &ffmpeg)
{
    Path p = ffmpeg;
    wchar_t *slash = wcsrchr(p.s, L'\\');
    wcscpy_s(slash ? slash + 1 : p.s, MAX_PATH - (slash ? (slash + 1 - p.s) : 0), L"ffprobe.exe");
    return p;
}

// ---------------------------------------------------------------------------------------------------------------
// Worker threads

static HANDLE thread_start(DWORD (WINAPI *fn)(void *), void *arg)
{
    HANDLE t = CreateThread(0, 0, fn, arg, 0, 0);
    CHECK(t, "CreateThread failed (%d)", (int)GetLastError());
    return t;
}

// ---------------------------------------------------------------------------------------------------------------
// What an ffmpeg needs to have. An ffmpeg.exe picked in Options is checked before it is used and refused with the
// reason it falls short, and Options shows the version of the one in use. A check is three short ffmpeg runs
// (-version, -filters, -encoders) on a worker thread.

static const i32 FFMPEG_MIN_MAJOR = 8; // gfxcapture, the default capture and the only one for a window, is 8.0's
// gfxcapture with ddagrab as the fallback, hwupload for the GPU encoders, the mix and the mic cleaning, the
// editor's fades, captions and title cards; H.264 on the CPU (the encoder fallback and the webcam), AAC and Opus.
static const char *const FFMPEG_FILTERS[] = {"gfxcapture", "ddagrab", "hwupload", "amix", "afftdn",
                                             "loudnorm", "xfade", "subtitles", "drawtext"};
static const char *const FFMPEG_ENCODERS[] = {"libx264", "aac", "libopus"};

// The version from -version's first line: "ffmpeg version 8.1.1-full_build-www.gyan.dev ..." -> "8.1.1" and 8,
// "n7.1-..." -> "7.1" and 7. A git or dated build has no number: 0, and `shown` is its version word, cut short.
// -1 if the text isn't ffmpeg's.
static i32 ffmpeg_version(const char *text, char *shown, i32 cap)
{
    shown[0] = 0;
    const char *v = strstr(text, "ffmpeg version ");
    if (!v) return -1;
    v += 15;
    const char *num = v[0] == 'n' && isdigit((u8)v[1]) ? v + 1 : v;
    char *end = 0;
    long major = strtol(num, &end, 10);
    if (end != num && *end == '.') {
        i32 n = 0;
        while (n < cap - 1 && (isdigit((u8)num[n]) || num[n] == '.')) n++;
        snprintf(shown, cap, "%.*s", n, num);
        return (i32)major;
    }
    i32 n = 0;
    while (v[n] && v[n] != ' ' && v[n] != '\r' && v[n] != '\n') n++;
    snprintf(shown, cap, "%.*s", n < 20 ? n : 20, v);
    return 0;
}

// Whether -filters or -encoders output lists `name`: each line is flags, then the name.
static bool ffmpeg_lists(const char *text, const char *name)
{
    size_t len = strlen(name);
    for (const char *p = text; *p;) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        const char *t = p;
        while (t < e && *t == ' ') t++;
        while (t < e && *t != ' ') t++;
        while (t < e && *t == ' ') t++;
        if ((size_t)(e - t) >= len && !memcmp(t, name, len) && (t + len == e || t[len] == ' ' || t[len] == '\r'))
            return true;
        p = *e ? e + 1 : e;
    }
    return false;
}

// Whether an ffmpeg with this -version, -filters and -encoders output (and ffprobe.exe beside it or not) is good
// enough. `shown` gets its version either way, `why` the first way it falls short.
static bool ffmpeg_judge(const char *version, const char *filters, const char *encoders, bool probe, char *shown,
                         i32 shown_cap, char *why, i32 why_cap)
{
    why[0] = 0;
    i32 major = ffmpeg_version(version, shown, shown_cap);
    if (major < 0) {
        snprintf(why, why_cap, "it didn't answer as ffmpeg does");
        return false;
    }
    if (major > 0 && major < FFMPEG_MIN_MAJOR) {
        snprintf(why, why_cap, "too old, " APP_NAME " needs ffmpeg %d or later", FFMPEG_MIN_MAJOR);
        return false;
    }
    char missing[192] = "";
    for (const char *f : FFMPEG_FILTERS)
        if (!ffmpeg_lists(filters, f)) snprintf(missing + strlen(missing), sizeof missing - strlen(missing), "%s%s", missing[0] ? ", " : "", f);
    for (const char *f : FFMPEG_ENCODERS)
        if (!ffmpeg_lists(encoders, f)) snprintf(missing + strlen(missing), sizeof missing - strlen(missing), "%s%s", missing[0] ? ", " : "", f);
    if (missing[0]) {
        snprintf(why, why_cap, "it has no %s", missing);
        return false;
    }
    if (!probe) {
        snprintf(why, why_cap, "ffprobe.exe isn't next to it");
        return false;
    }
    return true;
}

struct FfCheck {
    Path exe;             // the ffmpeg.exe checked last, or being checked
    volatile LONG busy;
    bool done, ok;        // read only while not busy
    char version[32];
    char why[192];        // what it lacks, when not ok
    HANDLE thread;
};

static DWORD WINAPI ffcheck_thread(void *arg)
{
    FfCheck *k = (FfCheck *)arg;
    const i32 cap = 1 << 18;
    char *v = (char *)calloc(1, cap), *f = (char *)calloc(1, cap), *e = (char *)calloc(1, cap);
    if (proc_run(k->exe.s, L"-hide_banner -version", 10000, v, cap) != 0) v[0] = 0;
    proc_run(k->exe.s, L"-hide_banner -filters", 10000, f, cap);
    proc_run(k->exe.s, L"-hide_banner -encoders", 10000, e, cap);
    Path probe = ffprobe_for(k->exe);
    k->ok = ffmpeg_judge(v, f, e, GetFileAttributesW(probe.s) != INVALID_FILE_ATTRIBUTES, k->version,
                         sizeof k->version, k->why, sizeof k->why);
    log_line("ffmpeg %ls: %s%s%s", k->exe.s, k->version[0] ? k->version : "no version", k->ok ? "" : ", ", k->why);
    free(v);
    free(f);
    free(e);
    k->done = true;
    InterlockedExchange(&k->busy, 0);
    return 0;
}

// Starts checking `exe`. False while a check is still running.
static bool ffcheck_start(FfCheck *k, const Path &exe)
{
    if (k->busy) return false;
    if (k->thread) {
        WaitForSingleObject(k->thread, INFINITE); // it has cleared busy, so it's ending
        CloseHandle(k->thread);
    }
    k->exe = exe;
    k->done = false;
    InterlockedExchange(&k->busy, 1);
    k->thread = thread_start(ffcheck_thread, k);
    return true;
}

// The finished check of `exe`; null while it runs, or when the last one checked was another file.
static const FfCheck *ffcheck_of(const FfCheck &k, const Path &exe)
{
    return !k.busy && k.done && !_wcsicmp(k.exe.s, exe.s) ? &k : 0;
}
