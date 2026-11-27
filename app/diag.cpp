// Diagnostics: stack traces in the log for crashes and hangs, a watchdog that notices a stuck main thread, and the
// minidumps both write. Dev mode (the detailed log) is in base.h; what it logs is spread through the code as
// DEV_LOG and dev_click (ui.cpp).

struct Diag {
    SRWLOCK        sym_lock;   // dbghelp is single threaded
    bool           sym_ready;
    HANDLE         main_thread;
    volatile i64   beat_ms;    // when the main thread last got round its loop or handled a message
    volatile LONG  waiting;    // 1 while the main thread sleeps in GetMessage: idle, not stuck
    // A copy of a suspended thread's stack, so it can be walked after the thread runs on (walking it while
    // suspended could deadlock on a lock it holds: the heap's, the log's, dbghelp's).
    u64            snap_base;
    u32            snap_size;
    u8             snap[256 * 1024];
};
static Diag g_diag;

static bool dev_env()
{
    wchar_t v[8] = {};
    return GetEnvironmentVariableW(L"REKODR_DEV", v, COUNT(v)) && v[0] && wcscmp(v, L"0");
}

static void diag_beat() { g_diag.beat_ms = (i64)GetTickCount64(); }

// Every window procedure runs inside this. A message handled there is work even when it arrives while the main
// loop sleeps in GetMessage (a sent message, a modal loop's timer), so it counts as busy until it returns.
template <LRESULT (*proc)(HWND, UINT, WPARAM, LPARAM)>
static LRESULT CALLBACK diag_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LONG waiting = g_diag.waiting;
    g_diag.waiting = 0;
    diag_beat();
    LRESULT r = proc(hwnd, msg, wp, lp);
    diag_beat();
    g_diag.waiting = waiting;
    return r;
}

static void diag_init()
{
    InitializeSRWLock(&g_diag.sym_lock);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_diag.main_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    diag_beat();
    // Symbols from rekodr.pdb next to the exe (build.bat writes it there), loaded the first time a stack is walked.
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(0, dir, MAX_PATH);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    g_diag.sym_ready = SymInitializeW(GetCurrentProcess(), dir, TRUE) != 0;
    if (!g_diag.sym_ready) log_line("stack traces won't be logged: SymInitialize failed (%lu)", GetLastError());
}

// Reads for StackWalk64: from the stack copy where it covers the address, else from memory.
static BOOL CALLBACK diag_read(HANDLE process, DWORD64 addr, PVOID out, DWORD size, LPDWORD got)
{
    if (g_diag.snap_size && addr >= g_diag.snap_base && addr + size <= g_diag.snap_base + g_diag.snap_size) {
        memcpy(out, g_diag.snap + (addr - g_diag.snap_base), size);
        if (got) *got = size;
        return TRUE;
    }
    SIZE_T n = 0;
    BOOL ok = ReadProcessMemory(process, (void *)addr, out, size, &n);
    if (got) *got = (DWORD)n;
    return ok;
}

// Logs the stack of `thread` from `ctx`, one frame per line: module!function+offset  file:line.
static void diag_log_stack(HANDLE thread, CONTEXT ctx, bool from_snapshot)
{
    if (!g_diag.sym_ready) return;
    AcquireSRWLockExclusive(&g_diag.sym_lock);
    if (!from_snapshot) g_diag.snap_size = 0;
    HANDLE proc = GetCurrentProcess();
    STACKFRAME64 f = {};
    f.AddrPC.Offset = ctx.Rip;
    f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp;
    f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp;
    f.AddrStack.Mode = AddrModeFlat;
    for (i32 n = 0; n < 64; n++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &f, &ctx, diag_read, SymFunctionTableAccess64,
                         SymGetModuleBase64, 0))
            break;
        DWORD64 pc = f.AddrPC.Offset;
        if (!pc) break;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 off = 0;
        char func[320] = "?";
        if (SymFromAddr(proc, pc, &off, sym)) snprintf(func, sizeof func, "%s+0x%llx", sym->Name, off);
        IMAGEHLP_MODULE64 mod = {sizeof mod};
        const char *mod_name = SymGetModuleInfo64(proc, pc, &mod) ? mod.ModuleName : "?";
        IMAGEHLP_LINE64 line = {sizeof line};
        DWORD disp = 0;
        if (SymGetLineFromAddr64(proc, pc, &disp, &line) && line.FileName) {
            const char *file = strrchr(line.FileName, '\\');
            log_line("  #%d %s!%s  %s:%lu", n, mod_name, func, file ? file + 1 : line.FileName, line.LineNumber);
        } else {
            log_line("  #%d %s!%s  (0x%llx)", n, mod_name, func, pc);
        }
    }
    g_diag.snap_size = 0;
    ReleaseSRWLockExclusive(&g_diag.sym_lock);
}

// A minidump in the local folder named <kind>-<date>-<time>.dmp: every thread's stack and the memory it points to. info is 0 for a dump that isn't a crash (a hang).
static Path diag_dump(EXCEPTION_POINTERS *info, const wchar_t *kind)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"%s-%04d%02d%02d-%02d%02d%02d.dmp", kind, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    Path dump = path_join(g_app.paths.local_dir, name);
    HANDLE f = CreateFileW(dump.s, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) {
        log_line("couldn't write a dump to %ls (%lu)", dump.s, GetLastError());
        return dump;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), info, FALSE};
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                         MiniDumpWithUnloadedModules | MiniDumpWithHandleData);
    if (!MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, info ? &mei : 0, 0, 0))
        log_line("MiniDumpWriteDump failed (0x%08lx)", GetLastError());
    CloseHandle(f);
    return dump;
}

// The crash reports waiting to be sent (Options > Privacy > Send crash reports) are in <local_dir>\reports, a pair
// each: <kind>-<date>-<time>.dmp, a small dump with no memory but the stack pointers and no folder names, and .txt,
// a few facts and the log's last lines as they were. telemetry.cpp scrubs the lines and sends the pair.
static const i32 REPORT_LOG_LINES = 200;

static Path diag_reports_dir() { return path_join(g_app.paths.local_dir, L"reports"); }

// The last `lines` lines of the file into out (at most cap - 1 bytes); their length. No heap: it runs in a crash.
static i32 diag_file_tail(const wchar_t *path, i32 lines, char *out, i32 cap)
{
    out[0] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size = {}, at = {};
    GetFileSizeEx(f, &size);
    at.QuadPart = size.QuadPart > cap - 1 ? size.QuadPart - (cap - 1) : 0;
    SetFilePointerEx(f, at, 0, FILE_BEGIN);
    DWORD n = 0;
    if (!ReadFile(f, out, (DWORD)(cap - 1), &n, 0)) n = 0;
    CloseHandle(f);
    out[n] = 0;
    i32 start = 0, seen = 0;
    for (i32 i = (i32)n - 2; i >= 0; i--) // from the end, not counting a last newline
        if (out[i] == '\n' && ++seen == lines) {
            start = i + 1;
            break;
        }
    if (!start && at.QuadPart > 0) { // the first line read is cut
        char *nl = strchr(out, '\n');
        start = nl ? (i32)(nl + 1 - out) : (i32)n;
    }
    memmove(out, out + start, n - start + 1);
    return (i32)n - start;
}

// Writes a report pair for a crash (info) or a hang (info 0). The log is flushed first so its last lines are there.
static void diag_report(EXCEPTION_POINTERS *info, const wchar_t *kind)
{
    static char text[96 * 1024];
    Path dir = diag_reports_dir();
    CreateDirectoryW(dir.s, 0);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"%s-%04d%02d%02d-%02d%02d%02d", kind, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    Path base = path_join(dir, name);
    Path dmp = base, txt = base;
    wcscat_s(dmp.s, L".dmp");
    wcscat_s(txt.s, L".txt");
    HANDLE f = CreateFileW(dmp.s, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), info, FALSE};
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpFilterMemory | MiniDumpFilterModulePaths |
                                         MiniDumpWithUnloadedModules);
    bool ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, info ? &mei : 0, 0, 0) != 0;
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(dmp.s);
        return;
    }
    // Where: the module the crash was in and how far into it
    char where[96] = "";
    if (info) {
        void *pc = info->ExceptionRecord->ExceptionAddress;
        HMODULE mod = 0;
        wchar_t mpath[MAX_PATH] = L"";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)pc, &mod) && GetModuleFileNameW(mod, mpath, MAX_PATH)) {
            const wchar_t *slash = wcsrchr(mpath, L'\\');
            snprintf(where, sizeof where, "%ls+0x%llx", slash ? slash + 1 : mpath, (u64)((u8 *)pc - (u8 *)mod));
        }
    }
    FILETIME made, gone, kt, ut, now;
    GetProcessTimes(GetCurrentProcess(), &made, &gone, &kt, &ut);
    GetSystemTimeAsFileTime(&now);
    i64 up = (i64)((((u64)now.dwHighDateTime << 32 | now.dwLowDateTime) - ((u64)made.dwHighDateTime << 32 | made.dwLowDateTime)) / 10000000);
    log_flush();
    i32 n = snprintf(text, sizeof text, "kind %ls\ncode 0x%08x\nwhere %s\nuptime %lld\nlog\n", kind,
                     info ? (u32)info->ExceptionRecord->ExceptionCode : 0, where, up);
    n += diag_file_tail(g_log_path, REPORT_LOG_LINES, text + n, (i32)sizeof text - n);
    f = CreateFileW(txt.s, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) {
        DeleteFileW(dmp.s);
        return;
    }
    DWORD w = 0;
    WriteFile(f, text, (DWORD)n, &w, 0);
    CloseHandle(f);
}

// Suspends the main thread just long enough to copy its registers and stack, then logs where it was.
static void diag_log_main_stack()
{
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    AcquireSRWLockExclusive(&g_diag.sym_lock); // the snapshot is shared with diag_log_stack
    bool got = false;
    if (SuspendThread(g_diag.main_thread) != (DWORD)-1) {
        got = GetThreadContext(g_diag.main_thread, &ctx) != 0;
        g_diag.snap_size = 0;
        if (got) { // the committed stack above rsp: one VirtualQuery region
            MEMORY_BASIC_INFORMATION mb = {};
            if (VirtualQuery((void *)ctx.Rsp, &mb, sizeof mb)) {
                u64 end = (u64)mb.BaseAddress + mb.RegionSize, size = end - ctx.Rsp;
                if (size > sizeof g_diag.snap) size = sizeof g_diag.snap;
                SIZE_T n = 0;
                if (ReadProcessMemory(GetCurrentProcess(), (void *)ctx.Rsp, g_diag.snap, (SIZE_T)size, &n)) {
                    g_diag.snap_base = ctx.Rsp;
                    g_diag.snap_size = (u32)n;
                }
            }
        }
        ResumeThread(g_diag.main_thread);
    }
    ReleaseSRWLockExclusive(&g_diag.sym_lock);
    // Another walker could take the lock in between and clear the copy; then the walk reads live memory instead.
    if (got) diag_log_stack(g_diag.main_thread, ctx, true);
    else log_line("  (couldn't read its registers: %lu)", GetLastError());
}

// The watchdog: if the main thread hasn't come round for 1 s while not idle, log where it is; at 15 s write a dump.
// A second already reads as a freeze, and the stack walk is cheap.
static DWORD WINAPI diag_watchdog(void *)
{
    bool stuck = false, dumped = false;
    i64 since = 0;
    for (;;) {
        Sleep(500);
        if (g_app.quitting) return 0;
        i64 now = (i64)GetTickCount64(), age = now - g_diag.beat_ms;
        bool busy = !g_diag.waiting && age > 1000;
        if (busy && !stuck) {
            stuck = true;
            dumped = false;
            since = g_diag.beat_ms;
            log_line("the main thread hasn't responded for %lld ms; it is at:", age);
            diag_log_main_stack();
        } else if (busy && !dumped && age > 15000) {
            dumped = true;
            log_line("still stuck after %lld ms, now at:", age);
            diag_log_main_stack();
            Path dump = diag_dump(0, L"hang");
            log_line("dump: %ls", dump.s);
            diag_report(0, L"hang");
        } else if (!busy && stuck) {
            stuck = false;
            log_line("the main thread is responding again (stuck for %lld ms)", g_diag.beat_ms - since);
        }
    }
}

static void diag_watchdog_start()
{
    HANDLE t = CreateThread(0, 0, diag_watchdog, 0, 0, 0);
    if (t) {
        SetThreadDescription(t, L"" APP_NAME " watchdog");
        CloseHandle(t);
    }
}

// Parts of a frame, timed so a slow frame can say where its time went. A part is timed by a FRAME_PART("name")
// in its scope; the totals per name are kept for the frame and logged only when it was slow.
struct FramePartTotal { const char *name; i64 ticks; i32 calls; };
static FramePartTotal g_frame_parts[24];
static i32 g_frame_part_count;

static i64 qpc_now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void frame_parts_reset() { g_frame_part_count = 0; }

static void frame_part_add(const char *name, i64 ticks)
{
    for (i32 i = 0; i < g_frame_part_count; i++)
        if (g_frame_parts[i].name == name || !strcmp(g_frame_parts[i].name, name)) {
            g_frame_parts[i].ticks += ticks;
            g_frame_parts[i].calls++;
            return;
        }
    if (g_frame_part_count < COUNT(g_frame_parts)) g_frame_parts[g_frame_part_count++] = {name, ticks, 1};
}

struct FramePartTimer {
    const char *name;
    i64         t0;
    FramePartTimer(const char *n) : name(n), t0(qpc_now()) {}
    ~FramePartTimer() { frame_part_add(name, qpc_now() - t0); }
};
#define FRAME_PART_CAT2(a, b) a##b
#define FRAME_PART_CAT(a, b) FRAME_PART_CAT2(a, b)
#define FRAME_PART(name) FramePartTimer FRAME_PART_CAT(frame_part_, __LINE__)(name)

// The timed parts of the frame just drawn, slowest first, as "player 310 ms (2), uploads 120 ms (14)".
static void frame_parts_text(char *out, i32 cap)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    FramePartTotal sorted[COUNT(g_frame_parts)];
    i32 n = g_frame_part_count;
    memcpy(sorted, g_frame_parts, n * sizeof sorted[0]);
    for (i32 i = 1; i < n; i++)
        for (i32 j = i; j > 0 && sorted[j].ticks > sorted[j - 1].ticks; j--) {
            FramePartTotal t = sorted[j];
            sorted[j] = sorted[j - 1];
            sorted[j - 1] = t;
        }
    out[0] = 0;
    for (i32 i = 0; i < n; i++) {
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%s%s %.0f ms (%d)", i ? ", " : "", sorted[i].name,
                 sorted[i].ticks * 1000.0 / f.QuadPart, sorted[i].calls);
    }
    if (!n) snprintf(out, cap, "none of the timed parts");
}
