// Basics shared by every part of rekodr: integer types, fixed-size strings, the log and CHECK.
//
// Code rules (see docs/SPEC.md, "Code"): plain structs that are valid when zeroed, functions that work on them, no
// class hierarchies or RAII wrappers, fixed arrays where the maximum is known.

typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;

#define COUNT(a) ((i32)(sizeof(a) / sizeof((a)[0])))

// ---------------------------------------------------------------------------------------------------------------
// Log: %LOCALAPPDATA%\Rekodr\rekodr-native.log (see paths_init), one line per call, also to the debugger.
//
// A call only queues its line; the log thread writes the queue to the file. A slow disk (a scan of the log, a busy
// VM) then never holds up the thread that logged, the UI's among them. log_flush writes what's queued at once: on a
// crash and at exit.

static SRWLOCK            g_log_lock;      // the queue
static SRWLOCK            g_log_file_lock; // the file: one writer at a time, so lines keep their order
static CONDITION_VARIABLE g_log_wake;
static wchar_t            g_log_path[MAX_PATH];
static char               g_log_queue[512 * 1024];
static int                g_log_queued;
static int                g_log_lost; // lines that didn't fit while the disk was stalled
static INIT_ONCE          g_log_once = INIT_ONCE_STATIC_INIT;

// Takes the queue and appends it to the file (dropped while there's no file yet). Must hold g_log_file_lock.
static void log_write_queued()
{
    static char batch[sizeof g_log_queue + 128];
    AcquireSRWLockExclusive(&g_log_lock);
    int n = g_log_queued, lost = g_log_lost;
    memcpy(batch, g_log_queue, n);
    g_log_queued = 0;
    g_log_lost = 0;
    ReleaseSRWLockExclusive(&g_log_lock);
    if (lost) n += snprintf(batch + n, 128, "(%d log lines lost: the log file couldn't keep up)\r\n", lost);
    if (!n || !g_log_path[0]) return;
    HANDLE f = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(f, batch, (DWORD)n, &written, 0);
    CloseHandle(f);
}

static DWORD WINAPI log_thread(void *)
{
    for (;;) {
        AcquireSRWLockExclusive(&g_log_lock);
        while (!g_log_queued && !g_log_lost) SleepConditionVariableSRW(&g_log_wake, &g_log_lock, INFINITE, 0);
        ReleaseSRWLockExclusive(&g_log_lock);
        AcquireSRWLockExclusive(&g_log_file_lock);
        log_write_queued();
        ReleaseSRWLockExclusive(&g_log_file_lock);
    }
}

// Writes what's queued now. Gives up after 2 s if the log thread is stuck writing (it may be the one crashing).
static void log_flush()
{
    for (int i = 0; !TryAcquireSRWLockExclusive(&g_log_file_lock); i++) {
        if (i == 200) return;
        Sleep(10);
    }
    log_write_queued();
    ReleaseSRWLockExclusive(&g_log_file_lock);
}

static BOOL CALLBACK log_start(INIT_ONCE *, void *, void **)
{
    HANDLE t = CreateThread(0, 0, log_thread, 0, 0, 0);
    if (t) {
        SetThreadDescription(t, L"log");
        CloseHandle(t);
    }
    atexit(log_flush);
    return TRUE;
}

// Dev mode: a detailed log (every click, key, screen change, command and event) for finding what led up to a
// problem. On with Options > General > Detailed log, `rekodr.exe dev`, or REKODR_DEV=1.
static volatile bool g_dev;
#define DEV_LOG(...) do { if (g_dev) log_line(__VA_ARGS__); } while (0)

static void log_line(const char *fmt, ...)
{
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200], thread[16] = "";
    if (g_dev) snprintf(thread, sizeof thread, " t%lu", GetCurrentThreadId()); // which thread, in dev mode
    int n = snprintf(line, sizeof line, "[%04d-%02d-%02d %02d:%02d:%02d.%03d%s] %s\r\n", t.wYear, t.wMonth, t.wDay,
                     t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, thread, msg);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    OutputDebugStringA(line);

    InitOnceExecuteOnce(&g_log_once, log_start, 0, 0);
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log_queued + n <= (int)sizeof g_log_queue) {
        memcpy(g_log_queue + g_log_queued, line, n);
        g_log_queued += n;
    } else {
        g_log_lost++;
    }
    ReleaseSRWLockExclusive(&g_log_lock);
    WakeConditionVariable(&g_log_wake);
}

// CHECK states what was expected and what was found. A failed CHECK logs where and why, then crashes so the
// crash handler writes a minidump and restarts Rekodr (main.cpp).
#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        if (!(cond)) {                                                                                            \
            log_line("CHECK failed at %s:%d: %s", __FILE__, __LINE__, #cond);                                     \
            log_line(__VA_ARGS__);                                                                                \
            __debugbreak();                                                                                       \
        }                                                                                                         \
    } while (0)

// ---------------------------------------------------------------------------------------------------------------
// Fixed-size strings. UTF-8 for anything shown or stored in our files, UTF-16 only at Windows API calls.

struct Str64  { char s[64]; };
struct Str256 { char s[256]; };
struct Path   { wchar_t s[MAX_PATH]; };

template <int N> static void str_set(char (&dst)[N], const char *src)
{
    snprintf(dst, N, "%s", src ? src : "");
}

template <int N> static bool str_eq(const char (&a)[N], const char *b)
{
    return strcmp(a, b) == 0;
}

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void utf8_to_wide(const char *src, wchar_t *dst, int dst_count)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dst_count);
    if (n == 0) dst[0] = 0;
}

static void wide_to_utf8(const wchar_t *src, char *dst, int dst_count)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_count, 0, 0);
    if (n == 0) dst[0] = 0;
}

// dir\name; a dir that already ends in a separator (a drive's root, "D:\") gets no second one.
static Path path_join(const Path &dir, const wchar_t *name)
{
    Path p = {};
    size_t n = wcslen(dir.s);
    bool sep = n && (dir.s[n - 1] == L'\\' || dir.s[n - 1] == L'/');
    swprintf(p.s, MAX_PATH, sep ? L"%s%s" : L"%s\\%s", dir.s, name);
    return p;
}

// ---------------------------------------------------------------------------------------------------------------
// Time. Everything recorded is placed on the wall clock in microseconds since 1970 (UTC), like the buffer's files.

static i64 now_us()
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    i64 t = ((i64)ft.dwHighDateTime << 32) | ft.dwLowDateTime; // 100 ns since 1601
    return (t - 116444736000000000LL) / 10;
}

// FNV-1a, for cache keys and names made from a path.
static u64 fnv64(const void *data, size_t n, u64 h = 14695981039346656037ull)
{
    const u8 *b = (const u8 *)data;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// A short name for REKODR_HOME, so a test copy's mutex, window class and pipe are its own. 0 = not a test copy.
static u32 test_home_hash()
{
    wchar_t home[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"REKODR_HOME", home, MAX_PATH) || !home[0]) return 0;
    u32 h = 2166136261u;
    for (wchar_t *c = home; *c; c++) h = (h ^ (u32)towlower(*c)) * 16777619u;
    return h ? h : 1;
}

// Reads a whole file (up to 64 MB) into memory from malloc, zero-terminated; free() it. Other programs may be
// writing it. Returns 0 if it can't be read.
static char *read_file(const wchar_t *path, i64 *size_out)
{
    if (size_out) *size_out = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size = {};
    char *buf = 0;
    if (GetFileSizeEx(f, &size) && size.QuadPart <= (64 << 20)) {
        buf = (char *)malloc((size_t)size.QuadPart + 1);
        DWORD got = 0;
        if (buf && ReadFile(f, buf, (DWORD)size.QuadPart, &got, 0)) {
            buf[got] = 0;
            if (size_out) *size_out = got;
        } else {
            free(buf);
            buf = 0;
        }
    }
    CloseHandle(f);
    return buf;
}

static const char *const MONTH_NAMES[] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                          "September", "October", "November", "December"};
static const char *month_name(i32 month) { return MONTH_NAMES[(month + 11) % 12]; } // 1 = January; "%.3s" for Jan

// Parses a length like "30s", "2m", "1h", "90" (seconds). Returns 0 if it isn't one.
static i64 parse_length_secs(const char *s)
{
    char *end = 0;
    double v = strtod(s, &end);
    if (end == s || v <= 0) return 0;
    switch (*end) {
    case 0: case 's': return (i64)v;
    case 'm': return (i64)(v * 60);
    case 'h': return (i64)(v * 3600);
    default: return 0;
    }
}

// Parses a range "<from> <to>" in Unix seconds, each with an optional fraction, into us on now_us()'s clock.
// Returns 1 for a range, 0 for a single word (not a range: a length, say) and -1 for two words that aren't one.
static i32 parse_range_us(const char *s, i64 *from_us, i64 *to_us)
{
    while (*s == ' ') s++;
    const char *gap = strchr(s, ' ');
    if (!gap) return 0;
    double v[2];
    const char *at = s;
    for (i32 i = 0; i < 2; i++) {
        while (*at == ' ') at++;
        char *end = 0;
        v[i] = strtod(at, &end);
        if (end == at || v[i] <= 0 || (*end && *end != ' ')) return -1;
        at = end;
    }
    while (*at == ' ') at++;
    if (*at) return -1;
    *from_us = (i64)llround(v[0] * 1e6);
    *to_us = (i64)llround(v[1] * 1e6);
    return *to_us > *from_us ? 1 : -1;
}
