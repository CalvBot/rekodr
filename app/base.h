// Basics shared by every part of clipback: integer types, fixed-size strings, the log and CHECK.
//
// Code rules (see SPEC.md, "Code"): plain structs that are valid when zeroed, functions that work on them, no
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
// Log: %LOCALAPPDATA%\clipback\clipback-native.log (see paths_init), one line per call, also to the debugger.

static SRWLOCK g_log_lock;
static wchar_t g_log_path[MAX_PATH];

// Dev mode: a detailed log (every click, key, screen change, command and event) for finding what led up to a
// problem. On with Options > General > Detailed log, `clipback.exe dev`, or CLIPBACK_DEV=1.
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

    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log_path[0]) {
        HANDLE f = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, 0);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(f, line, (DWORD)n, &written, 0);
            CloseHandle(f);
        }
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

// CHECK states what was expected and what was found. A failed CHECK logs where and why, then crashes so the
// crash handler writes a minidump and restarts clipback (main.cpp).
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

static Path path_join(const Path &dir, const wchar_t *name)
{
    Path p = {};
    swprintf(p.s, MAX_PATH, L"%s\\%s", dir.s, name);
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
