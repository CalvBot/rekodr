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
    return GetEnvironmentVariableW(L"CLIPBACK_DEV", v, COUNT(v)) && v[0] && wcscmp(v, L"0");
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
    // Symbols from clipback.pdb next to the exe (build.bat writes it there), loaded the first time a stack is walked.
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
        SetThreadDescription(t, L"clipback watchdog");
        CloseHandle(t);
    }
}
