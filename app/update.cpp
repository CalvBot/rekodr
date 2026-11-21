// Updating itself. A background thread fetches the release manifest a minute after start and then once a day. When
// it names a newer version, the thread downloads that exe into local_dir\update, checking its size and SHA-256, and
// the update is then ready: update_status() gives its version, and update_apply() swaps it in and restarts.
//
// The manifest is text, one "key value" per line, with the signature last:
//   version 0.3.0
//   url https://github.com/CalvBot/clipback/releases/download/v0.3.0/clipback-0.3.0.exe
//   size 3502080
//   sha256 <64 hex digits>
//   sig <128 hex digits>
// sig is an ECDSA P-256 signature (r then s) over the SHA-256 of every byte before the "sig" line. The public key is
// built in (UPDATE_PUBLIC_KEY), so a manifest only counts if it was signed with the release key, and the exe only if
// it matches the manifest's size and hash.
//
// For tests, a test copy (CLIPBACK_HOME) may set CLIPBACK_UPDATE_MANIFEST (an http or https URL) and
// CLIPBACK_UPDATE_KEY (the public key, 128 hex digits, X then Y). `clipback.exe update-check` runs one check and
// download in the foreground, logs what happened and exits 0 when an update was made ready; `update-check apply`
// also swaps it in (a test copy doesn't start the new exe, which would open its window).

// TODO: the manifest's address, once it's decided where releases go. Empty means no checks.
static const wchar_t UPDATE_MANIFEST_URL[] = L"";
// TODO: the release key's public half (X then Y, 32 bytes each). All zeros verifies nothing.
static const u8 UPDATE_PUBLIC_KEY[64] = {};
static const i64 UPDATE_MAX_EXE = 64ll << 20;

enum UpdateState : LONG { UPDATE_IDLE, UPDATE_CHECKING, UPDATE_DOWNLOADING, UPDATE_READY, UPDATE_FAILED };

struct Update {
    volatile LONG state;
    char version[32]; // the ready version, set before state turns UPDATE_READY
    Path exe;         // the downloaded exe
    char error[160];
    HANDLE thread, wake;
    volatile LONG stop;
};
static Update g_update;
static u64 g_update_started_ms;           // when this copy, started by an update, began; 0 otherwise
static const u32 UPDATE_TRIAL_MS = 60000; // a crash this soon after an update puts the old exe back

struct UpdateManifest {
    char version[32];
    char url[512];
    i64 size;
    char sha256[65];
};

static bool update_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_update.error, sizeof g_update.error, fmt, ap);
    va_end(ap);
    log_line("update: %s", g_update.error);
    InterlockedExchange(&g_update.state, UPDATE_FAILED);
    return false;
}

// "0.12.3" against "0.9.0": -1, 0 or 1, part by part
static i32 update_compare(const char *a, const char *b)
{
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if ((*a && !isdigit((u8)*a)) || (*b && !isdigit((u8)*b))) break;
    }
    return 0;
}

static bool update_unhex(const char *hex, u8 *out, i32 n)
{
    if ((i32)strlen(hex) != n * 2) return false;
    for (i32 i = 0; i < n; i++) {
        u32 v = 0;
        if (sscanf_s(hex + i * 2, "%2x", &v) != 1) return false;
        out[i] = (u8)v;
    }
    return true;
}

static bool update_test_copy_override(const wchar_t *name, wchar_t *out, i32 cap)
{
    out[0] = 0;
    return is_test_copy() && GetEnvironmentVariableW(name, out, cap) && out[0];
}

// GETs a URL. With `file` the body streams there (at most `max` bytes, hashed into `hash`); otherwise it goes into
// `body` (at most max - 1 bytes, 0-terminated). Returns the body's length, or -1 after update_fail.
static i64 update_get(const wchar_t *url, i64 max, char *body, FILE *file, BCRYPT_HASH_HANDLE hash)
{
    URL_COMPONENTS uc = {sizeof uc};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = COUNT(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = COUNT(path);
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return update_fail("a bad address: %ls", url), -1;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && !is_test_copy()) return update_fail("not https: %ls", url), -1;
    i64 total = -1;
    HINTERNET session = WinHttpOpen(L"clipback", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host, uc.nPort, 0) : 0;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, 0, 0, 0,
                                              uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : 0;
    DWORD status = 0, len = sizeof status;
    if (req) WinHttpSetTimeouts(req, 15000, 15000, 30000, 60000);
    if (!req || !WinHttpSendRequest(req, 0, 0, 0, 0, 0, 0) || !WinHttpReceiveResponse(req, 0) ||
        !WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &status, &len, 0)) {
        update_fail("couldn't reach %ls (error %d)", host, (int)GetLastError());
    } else if (status != 200) {
        update_fail("%ls answered %u", host, (unsigned)status);
    } else {
        static char buf[1 << 16];
        total = 0;
        for (;;) {
            DWORD got = 0;
            if (g_update.stop) { total = -1; break; }
            if (!WinHttpReadData(req, buf, sizeof buf, &got)) {
                update_fail("the download broke off (error %d)", (int)GetLastError());
                total = -1;
                break;
            }
            if (!got) break;
            if (total + got > (file ? max : max - 1)) {
                update_fail("%ls sent more than %lld bytes", host, (long long)max);
                total = -1;
                break;
            }
            if (file) {
                if (fwrite(buf, 1, got, file) != got) { update_fail("the disk is full"); total = -1; break; }
                BCryptHashData(hash, (PUCHAR)buf, got, 0);
            } else {
                memcpy(body + total, buf, got);
            }
            total += got;
        }
        if (body && total >= 0) body[total] = 0;
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    return total;
}

static bool update_sha256(const void *data, size_t n, u8 out[32])
{
    BCRYPT_ALG_HANDLE alg = 0;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, 0, 0))) return false;
    bool ok = BCRYPT_SUCCESS(BCryptHash(alg, 0, 0, (PUCHAR)data, (ULONG)n, out, 32));
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// The signature over the text before the "sig" line, against the built-in key (or a test copy's)
static bool update_signed(const char *text, size_t signed_len, const char *sig_hex)
{
    u8 key[64];
    memcpy(key, UPDATE_PUBLIC_KEY, 64);
    wchar_t test_key[160];
    if (update_test_copy_override(L"CLIPBACK_UPDATE_KEY", test_key, COUNT(test_key))) {
        char k[160];
        wide_to_utf8(test_key, k, sizeof k);
        if (!update_unhex(k, key, 64)) return update_fail("CLIPBACK_UPDATE_KEY isn't 128 hex digits");
    }
    u8 zero[64] = {}, sig[64], digest[32];
    if (!memcmp(key, zero, 64)) return update_fail("no release key is built in");
    if (!update_unhex(sig_hex, sig, 64)) return update_fail("the manifest's sig isn't 128 hex digits");
    if (!update_sha256(text, signed_len, digest)) return update_fail("SHA-256 isn't available");
    struct { BCRYPT_ECCKEY_BLOB head; u8 xy[64]; } blob = {{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32}, {}};
    memcpy(blob.xy, key, 64);
    BCRYPT_ALG_HANDLE alg = 0;
    BCRYPT_KEY_HANDLE k = 0;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, 0, 0)) &&
              BCRYPT_SUCCESS(BCryptImportKeyPair(alg, 0, BCRYPT_ECCPUBLIC_BLOB, &k, (PUCHAR)&blob, sizeof blob, 0)) &&
              BCRYPT_SUCCESS(BCryptVerifySignature(k, 0, digest, 32, sig, 64, 0));
    if (k) BCryptDestroyKey(k);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok ? true : update_fail("the manifest's signature doesn't match the release key");
}

static bool update_parse(char *text, UpdateManifest *m)
{
    *m = {};
    char *sig_line = strstr(text, "\nsig ");
    if (!sig_line) return update_fail("the manifest isn't signed");
    char sig[160] = {};
    sscanf_s(sig_line + 5, "%159s", sig, (unsigned)sizeof sig);
    if (!update_signed(text, (size_t)(sig_line + 1 - text), sig)) return false;
    *sig_line = 0;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(0, "\r\n")) {
        char key[16] = {}, value[512] = {};
        if (sscanf_s(line, "%15s %511s", key, (unsigned)sizeof key, value, (unsigned)sizeof value) != 2) continue;
        if (!strcmp(key, "version")) str_set(m->version, value);
        else if (!strcmp(key, "url")) str_set(m->url, value);
        else if (!strcmp(key, "size")) m->size = _strtoi64(value, 0, 10);
        else if (!strcmp(key, "sha256")) str_set(m->sha256, value);
    }
    if (!m->version[0] || !m->url[0] || m->size <= 0 || m->size > UPDATE_MAX_EXE || strlen(m->sha256) != 64)
        return update_fail("the manifest is missing its version, url, size or sha256");
    return true;
}

// One check, and the download when there's a newer version. True when an update is ready.
static bool update_check_once()
{
    wchar_t url[600];
    if (!update_test_copy_override(L"CLIPBACK_UPDATE_MANIFEST", url, COUNT(url))) wcscpy_s(url, UPDATE_MANIFEST_URL);
    if (!url[0]) return false;
    InterlockedExchange(&g_update.state, UPDATE_CHECKING);
    static char text[8192];
    if (update_get(url, sizeof text, text, 0, 0) < 0) return false;
    UpdateManifest m;
    if (!update_parse(text, &m)) return false;
    if (update_compare(m.version, CLIPBACK_VERSION) <= 0) {
        log_line("update: %s is the newest (the manifest says %s)", CLIPBACK_VERSION, m.version);
        InterlockedExchange(&g_update.state, UPDATE_IDLE);
        return false;
    }
    Path dir = path_join(g_app.paths.local_dir, L"update"), part = {}, exe = {};
    char skip[32] = {};
    if (FILE *f = _wfopen(path_join(dir, L"skip.txt").s, L"r")) {
        if (!fgets(skip, sizeof skip, f)) skip[0] = 0;
        fclose(f);
    }
    skip[strcspn(skip, "\r\n")] = 0;
    if (!strcmp(skip, m.version)) {
        log_line("update: %s crashed here after an update, so it isn't fetched again", m.version);
        InterlockedExchange(&g_update.state, UPDATE_IDLE);
        return false;
    }
    SHCreateDirectoryExW(0, dir.s, 0);
    swprintf(exe.s, MAX_PATH, L"%s\\clipback-%hs.exe", dir.s, m.version);
    swprintf(part.s, MAX_PATH, L"%s.part", exe.s);
    log_line("update: %s is out, downloading it from %s", m.version, m.url);
    InterlockedExchange(&g_update.state, UPDATE_DOWNLOADING);
    BCRYPT_ALG_HANDLE alg = 0;
    BCRYPT_HASH_HANDLE hash = 0;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, 0, 0)) ||
        !BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, 0, 0, 0, 0, 0))) {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        return update_fail("SHA-256 isn't available");
    }
    wchar_t wurl[512];
    utf8_to_wide(m.url, wurl, COUNT(wurl));
    FILE *f = _wfopen(part.s, L"wb");
    i64 got = f ? update_get(wurl, m.size, 0, f, hash) : (update_fail("couldn't write %ls", part.s), -1);
    if (f) fclose(f);
    u8 h[32];
    char hex[65] = {};
    BCryptFinishHash(hash, h, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    hex32(h, hex);
    bool ok = got >= 0;
    if (ok && got != m.size) ok = update_fail("got %lld bytes, the manifest says %lld", (long long)got, (long long)m.size);
    if (ok && _stricmp(hex, m.sha256)) ok = update_fail("the download's sha256 is %s, the manifest says %s", hex, m.sha256);
    if (ok && !MoveFileExW(part.s, exe.s, MOVEFILE_REPLACE_EXISTING)) ok = update_fail("couldn't rename %ls", part.s);
    if (!ok) {
        DeleteFileW(part.s);
        return false;
    }
    g_update.exe = exe;
    str_set(g_update.version, m.version);
    InterlockedExchange(&g_update.state, UPDATE_READY);
    log_line("update: %s is ready (%ls)", m.version, exe.s);
    return true;
}

static DWORD WINAPI update_thread(void *)
{
    DWORD wait = 60 * 1000;
    while (!g_update.stop) {
        WaitForSingleObject(g_update.wake, wait);
        if (g_update.stop || update_check_once()) break;
        wait = 24 * 3600 * 1000;
    }
    return 0;
}

// From main at start, and when "Update by itself" is turned on
static void update_start()
{
    if (g_update.thread) {
        if (!g_update.stop || WaitForSingleObject(g_update.thread, 0) != WAIT_OBJECT_0) return; // running, or still stopping
        CloseHandle(g_update.thread);
        CloseHandle(g_update.wake);
        g_update.thread = g_update.wake = 0;
    }
    wchar_t test_url[600];
    bool test = update_test_copy_override(L"CLIPBACK_UPDATE_MANIFEST", test_url, COUNT(test_url));
    if (is_test_copy() ? !test : !UPDATE_MANIFEST_URL[0]) return; // a test copy checks only its own manifest
    InterlockedExchange(&g_update.stop, 0);
    g_update.wake = CreateEventW(0, FALSE, FALSE, 0);
    g_update.thread = CreateThread(0, 0, update_thread, 0, 0, 0);
}

// From main at exit, and when "Update by itself" is turned off. A thread stuck in a download past 5 s keeps its
// handles; update_start waits for it to go.
static void update_stop()
{
    if (!g_update.thread) return;
    InterlockedExchange(&g_update.stop, 1);
    SetEvent(g_update.wake);
    if (WaitForSingleObject(g_update.thread, 5000) != WAIT_OBJECT_0) return;
    CloseHandle(g_update.thread);
    CloseHandle(g_update.wake);
    g_update.thread = g_update.wake = 0;
}

// Why the last check, download or apply failed, or 0
static const char *update_error()
{
    return g_update.state == UPDATE_FAILED ? g_update.error : 0;
}

// True once a verified update is ready to swap in, with its version in `ver`
static bool update_status(char *ver, i32 cap)
{
    if (g_update.state != UPDATE_READY) return false;
    snprintf(ver, cap, "%s", g_update.version);
    return true;
}

// Swaps the ready exe in for this one and starts it, then quits. The new copy waits for this one to go (it's told
// the pid in CLIPBACK_AFTER_UPDATE), and deletes clipback.old.exe once it can (install_cleanup). The ffmpeg children
// are in this process's kill-on-close job, so recording stops for the few seconds until the new copy starts its own.
static bool update_apply(bool start_new = true)
{
    if (g_update.state != UPDATE_READY) return false;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(0, self, MAX_PATH);
    Path aside = {};
    wcscpy_s(aside.s, self);
    if (wchar_t *slash = wcsrchr(aside.s, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - aside.s), L"clipback.old.exe");
    if (!install_swap(g_update.exe.s, self, aside.s, true)) return update_fail("couldn't swap the new exe in");
    log_line("update: %s swapped in for %s", g_update.version, CLIPBACK_VERSION);
    if (!start_new) return true;
    wchar_t pid[16];
    swprintf(pid, COUNT(pid), L"%lu", GetCurrentProcessId());
    SetEnvironmentVariableW(L"CLIPBACK_AFTER_UPDATE", pid);
    bool started = install_start(self, g_app.ui.shown ? L"open" : L"");
    SetEnvironmentVariableW(L"CLIPBACK_AFTER_UPDATE", 0);
    if (!started) {
        MoveFileExW(aside.s, self, MOVEFILE_REPLACE_EXISTING); // the old one back; this copy carries on
        return update_fail("the new exe didn't start");
    }
    g_app.quitting = true;
    PostQuitMessage(0);
    return true;
}

// From main, before taking the single-instance mutex: a copy started by update_apply waits for the old one to go.
static void update_wait_for_old()
{
    wchar_t pid[16] = {};
    if (!GetEnvironmentVariableW(L"CLIPBACK_AFTER_UPDATE", pid, COUNT(pid)) || !pid[0]) return;
    SetEnvironmentVariableW(L"CLIPBACK_AFTER_UPDATE", 0);
    HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(pid, 0, 10));
    if (old) {
        WaitForSingleObject(old, 15000);
        CloseHandle(old);
    }
    g_update_started_ms = GetTickCount64();
}

// A file next to this exe
static Path update_beside(const wchar_t *name)
{
    Path p = {};
    GetModuleFileNameW(0, p.s, MAX_PATH);
    if (wchar_t *slash = wcsrchr(p.s, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - p.s), name);
    return p;
}

// From main at start, in place of install_cleanup: a copy an update started keeps clipback.old.exe for its first
// minute, so update_rollback can put it back.
static void update_cleanup()
{
    if (!g_update_started_ms) {
        install_cleanup();
        return;
    }
    HANDLE t = CreateThread(0, 0, [](void *) -> DWORD { Sleep(UPDATE_TRIAL_MS); install_cleanup(); return 0; }, 0, 0, 0);
    if (t) CloseHandle(t);
}

// From the crash handler. A copy an update started that crashes within its first minute puts the exe it replaced
// back, and writes its own version to update\skip.txt so checks pass it by. True when the old exe is back.
static bool update_rollback()
{
    if (!g_update_started_ms || GetTickCount64() - g_update_started_ms >= UPDATE_TRIAL_MS) return false;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(0, self, MAX_PATH);
    Path old = update_beside(L"clipback.old.exe"), bad = update_beside(L"clipback.bad.exe");
    if (GetFileAttributesW(old.s) == INVALID_FILE_ATTRIBUTES) return false;
    if (!install_swap(old.s, self, bad.s, true)) return false;
    MoveFileExW(bad.s, old.s, MOVEFILE_REPLACE_EXISTING); // the next start deletes it
    Path dir = path_join(g_app.paths.local_dir, L"update");
    SHCreateDirectoryExW(0, dir.s, 0);
    if (FILE *f = _wfopen(path_join(dir, L"skip.txt").s, L"w")) {
        fprintf(f, "%s\n", CLIPBACK_VERSION);
        fclose(f);
    }
    log_line("update: %s crashed in its first minute; the exe it replaced is back", CLIPBACK_VERSION);
    return true;
}

// For main: `update-check [apply]`, one check in the foreground for tests. True if it was that command.
static bool update_command(const char *command, i32 *code)
{
    if (!starts_with(command, "update-check")) return false;
    paths_init(&g_app.paths);
    bool ok = update_check_once();
    if (ok && strstr(command, "apply")) ok = update_apply(!is_test_copy());
    log_line("update-check: %s", ok ? "ready" : g_update.error[0] ? g_update.error : "nothing newer");
    *code = ok ? 0 : 1;
    return true;
}
