// Fetching ffmpeg. When no ffmpeg is found (Options path, PATH, winget), clipback downloads one pinned build on its
// own: Gyan's 8.1.1 full shared build from its GitHub release. The zip is checked against its size and SHA-256
// while it streams in, then unpacked with Windows' own tar.exe into ffget_dir() (local_dir\ffmpeg). ffmpeg_find
// picks it up from there, so the recorder (which retries every 5 s) starts by itself once it lands.
// The build is GPL; its LICENSE file is kept next to it.

static const wchar_t FFGET_HOST[] = L"github.com";
static const wchar_t FFGET_PATH[] =
    L"/GyanD/codexffmpeg/releases/download/8.1.1/ffmpeg-8.1.1-full_build-shared.zip";
static const char FFGET_SHA256[] = "4296b396bdfd5fbc3dfc75ab4c8703354a56963232d65c4182993543df2d2f45";
static const i64 FFGET_BYTES = 101083228;

enum FfgetState : LONG { FFGET_IDLE, FFGET_RUNNING, FFGET_DONE, FFGET_FAILED };

struct Ffget {
    volatile LONG state;
    volatile LONG64 got; // bytes downloaded so far
    char error[160];     // set before state turns FFGET_FAILED
    HANDLE thread;
    volatile LONG stop;
};
static Ffget g_ffget;

static void ffget_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_ffget.error, sizeof g_ffget.error, fmt, ap);
    va_end(ap);
    log_line("ffget: %s", g_ffget.error);
    InterlockedExchange(&g_ffget.state, FFGET_FAILED);
}

static void hex32(const u8 h[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (i32 i = 0; i < 32; i++) {
        out[i * 2] = digits[h[i] >> 4];
        out[i * 2 + 1] = digits[h[i] & 15];
    }
    out[64] = 0;
}

// Streams the zip into `file`, hashing as it goes. True only when the size and hash match the pinned build.
static bool ffget_download(const Path &file)
{
    bool ok = false;
    BCRYPT_ALG_HANDLE alg = 0;
    BCRYPT_HASH_HANDLE hash = 0;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, 0, 0)) ||
        !BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, 0, 0, 0, 0, 0))) {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        ffget_fail("SHA-256 isn't available");
        return false;
    }
    HINTERNET session = WinHttpOpen(L"clipback", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0);
    HINTERNET conn = session ? WinHttpConnect(session, FFGET_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0) : 0;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", FFGET_PATH, 0, 0, 0, WINHTTP_FLAG_SECURE) : 0;
    DWORD status = 0, len = sizeof status;
    if (req) WinHttpSetTimeouts(req, 15000, 15000, 30000, 60000);
    if (!req || !WinHttpSendRequest(req, 0, 0, 0, 0, 0, 0) || !WinHttpReceiveResponse(req, 0) ||
        !WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &status, &len, 0)) {
        ffget_fail("couldn't reach GitHub (error %d)", (int)GetLastError());
    } else if (status != 200) {
        ffget_fail("GitHub answered %u", (unsigned)status);
    } else {
        FILE *f = _wfopen(file.s, L"wb");
        if (!f) {
            ffget_fail("couldn't write %ls", file.s);
        } else {
            static char buf[1 << 16];
            i64 total = 0;
            ok = true;
            for (;;) {
                DWORD got = 0;
                if (g_ffget.stop) { ok = false; break; }
                if (!WinHttpReadData(req, buf, sizeof buf, &got)) {
                    ffget_fail("the download broke off (error %d)", (int)GetLastError());
                    ok = false;
                    break;
                }
                if (!got) break;
                total += got;
                if (total > FFGET_BYTES) { ffget_fail("the download is bigger than the pinned build"); ok = false; break; }
                if (fwrite(buf, 1, got, f) != got) { ffget_fail("the disk is full"); ok = false; break; }
                BCryptHashData(hash, (PUCHAR)buf, got, 0);
                InterlockedExchange64(&g_ffget.got, total);
            }
            fclose(f);
            if (ok && total != FFGET_BYTES) {
                ffget_fail("the download stopped at %lld of %lld bytes", (long long)total, (long long)FFGET_BYTES);
                ok = false;
            }
        }
    }
    if (ok) {
        u8 h[32];
        char hex[65];
        BCryptFinishHash(hash, h, 32, 0);
        hex32(h, hex);
        if (strcmp(hex, FFGET_SHA256) != 0) {
            ffget_fail("the download doesn't match the pinned build (sha256 %s)", hex);
            ok = false;
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static DWORD WINAPI ffget_thread(void *)
{
    Path dir = ffget_dir();
    SHCreateDirectoryExW(0, dir.s, 0);
    Path zip = path_join(dir, L"ffmpeg.zip.part");
    log_line("ffget: downloading ffmpeg 8.1.1 into %ls", dir.s);
    if (!ffget_download(zip)) {
        DeleteFileW(zip.s);
        if (g_ffget.stop) InterlockedExchange(&g_ffget.state, FFGET_IDLE);
        return 0;
    }
    wchar_t tar[MAX_PATH];
    GetSystemDirectoryW(tar, MAX_PATH);
    wcsncat_s(tar, COUNT(tar), L"\\tar.exe", _TRUNCATE);
    wchar_t args[2048] = {};
    cmd_arg(args, COUNT(args), L"-xf");
    cmd_arg(args, COUNT(args), zip.s);
    cmd_arg(args, COUNT(args), L"-C");
    cmd_arg(args, COUNT(args), dir.s);
    cmd_arg(args, COUNT(args), L"--exclude");
    cmd_arg(args, COUNT(args), L"*/ffplay.exe");
    char out[512];
    i32 code = proc_run(tar, args, 300000, out, sizeof out);
    DeleteFileW(zip.s);
    Path exe = ffget_exe();
    if (code != 0 || GetFileAttributesW(exe.s) == INVALID_FILE_ATTRIBUTES) {
        ffget_fail("unpacking failed (tar %d: %.100s)", (int)code, out);
        return 0;
    }
    log_line("ffget: ffmpeg is ready at %ls", exe.s);
    InterlockedExchange(&g_ffget.state, FFGET_DONE);
    return 0;
}

// Starts the download if no ffmpeg is found and none is on its way (or always when forced, for the fetch-ffmpeg
// command). Safe to call again (Retry).
static void ffget_start(bool force = false)
{
    Path found;
    if (!force && ffmpeg_find(g_app.config.ffmpeg, &found)) return;
    if (!force && g_app.config.ffmpeg.s[0]) return; // a path set in Options that isn't there: the player's to fix
    LONG s = g_ffget.state;
    if (s == FFGET_RUNNING || s == FFGET_DONE) return;
    if (g_ffget.thread) {
        WaitForSingleObject(g_ffget.thread, INFINITE);
        CloseHandle(g_ffget.thread);
    }
    g_ffget.error[0] = 0;
    InterlockedExchange64(&g_ffget.got, 0);
    InterlockedExchange(&g_ffget.state, FFGET_RUNNING);
    g_ffget.thread = thread_start(ffget_thread, 0);
}

// Called once a second on the main thread: once the download lands, the library scans again with ffmpeg.
static void ffget_tick()
{
    static bool announced;
    if (g_ffget.state == FFGET_DONE && !announced) {
        announced = true;
        library_rescan();
        ui_wake();
    }
}

static void ffget_stop()
{
    InterlockedExchange(&g_ffget.stop, 1);
    if (g_ffget.thread) {
        WaitForSingleObject(g_ffget.thread, 5000);
        CloseHandle(g_ffget.thread);
        g_ffget.thread = 0;
    }
}

// One line for the UI while ffmpeg is on its way or failed; false when there's nothing to say.
static bool ffget_status(char *out, i32 cap, f32 *progress)
{
    LONG s = g_ffget.state;
    *progress = (f32)((f64)g_ffget.got / (f64)FFGET_BYTES);
    if (s == FFGET_RUNNING) {
        snprintf(out, cap, "Getting ffmpeg, which clipback records with: %d of %d MB", (int)(g_ffget.got >> 20),
                 (int)(FFGET_BYTES >> 20));
        return true;
    }
    if (s == FFGET_FAILED) {
        snprintf(out, cap, "Couldn't get ffmpeg: %s", g_ffget.error);
        return true;
    }
    return false;
}
