// Links: a clip shared as a plain https link to an MP4 that Discord plays in place. Where it goes:
//   free hosts  with nothing set up, the copy goes to a free public host, the first in the player's order
//               (link_hosts) that takes its size: Catbox (up to 200 MB, kept, can't be deleted) or Litterbox (up to
//               1 GB, gone after litterbox_hours). Anyone with the link can watch. A host that fails passes the clip
//               to the next.
//   R2          once the player has set up their own Cloudflare R2 bucket, every clip goes there instead, as
//               clipback/<random folder>/clip.mp4, so a link names no player, game or date and can't be guessed.
//               The link points at the bucket's public address (r2.dev or the player's own domain).
//
// A share runs in three steps:
//   preparing  save_link (save.cpp) makes an H.264 copy with the Mix only, played from the start while it loads
//   uploading  the copy goes to the bucket over R2's S3 API (signed with AWS Signature V4), in parts when large,
//              or to a free host as a form upload; the copy is deleted after
//   cleanup    R2 only: while the bucket's clipback/ folder holds more than the player's limit (its real total,
//              listed after each upload), the oldest clips that aren't pinned are deleted from it; their history
//              entries stay, marked removed, and can be shared again under the same name
//
// The access key, account and bucket are in settings.txt; the secret key only in Windows' Credential Manager.
// History is <config_dir>\shares.tsv. Network work runs on one worker thread; the main thread owns the jobs'
// steps and is told by EV_R2. This file never touches the clipboard: the UI copies the link.

enum R2State : u8 { R2_QUEUED, R2_PREPARING, R2_UPLOADING, R2_DONE, R2_FAILED };
enum R2TestState : u8 { R2_TEST_IDLE, R2_TEST_BUSY, R2_TEST_OK, R2_TEST_FAILED };

struct R2Job {
    u32     id; // 0 = no such job
    R2State state;
    f32     progress; // 0..1 within the current step
    char    url[512]; // once done
    char    error[256];
};

struct R2View {
    R2TestState state;
    char        text[256]; // ready to show
};

enum { R2_MAX_SHARES = 1024, R2_KEY_MAX = 48 };

enum LinkHost : u8 { HOST_R2, HOST_CATBOX, HOST_LITTERBOX, HOST_COUNT };

// The free public hosts, used when R2 isn't set up.
struct FreeHost {
    LinkHost       host;
    const char    *name;  // as in link_hosts
    const char    *label; // for the player
    const wchar_t *server, *path;
    i64            max_bytes;
    bool           expires; // files go after litterbox_hours
};
static const FreeHost FREE_HOSTS[] = {
    {HOST_CATBOX, "catbox", "Catbox", L"catbox.moe", L"/user/api.php", 200000000, false},
    {HOST_LITTERBOX, "litterbox", "Litterbox", L"litterbox.catbox.moe", L"/resources/internals/api.php", 1000000000,
     true},
};

struct R2Share {
    char key[R2_KEY_MAX]; // R2: object name in the bucket, "clipback/<16 random letters and digits>/clip.mp4";
                          // a free host: "<host>/<the file's name there>"
    char url[512];    // the link as given out
    char title[256];
    char game[64];
    char path[1040];  // the file it was made from (UTF-8), for sharing it again
    i64  bytes;       // size in the bucket
    i64  uploaded_us;
    bool pinned;      // never removed to make room
    bool removed;     // deleted from the bucket to make room; the entry stays so it can be shared again
    i32  version;     // 1, then one more each time it's shared again; links past the first end in ?v=<version>
    u64  lib_key;     // the library item it was made from, 0 for an editor render
    LinkHost host;
    i64  expires_us;  // when a free host drops it, 0 = kept
};

struct R2Usage {
    i64 used_bytes, limit_bytes;
    f64 cost_per_gb_month, free_gb;
};

static const f64 R2_FREE_GB = 10, R2_PRICE_GB = 0.015; // Cloudflare's R2 storage: 10 GB free, then per GB-month
static const i64 R2_GB = 1000000000;
static const i64 R2_PART = 64ll << 20;       // multipart part size
static const i64 R2_SINGLE_MAX = 100ll << 20; // larger files go up in parts

enum R2EventKind : u8 { R2_EV_PROBED, R2_EV_JOB, R2_EV_TEST, R2_EV_HISTORY };

// What a network task needs of the settings, copied when the task is queued.
struct R2Conf {
    char account[40], access[72], secret[96], bucket[64], public_url[256];
    i64  limit;
};

enum R2TaskKind : u8 { R2_T_PROBE, R2_T_UPLOAD, R2_T_FREE, R2_T_DELETE, R2_T_TEST };

struct R2Task {
    R2TaskKind kind;
    i32        slot;    // the job's slot (probe, upload, free)
    char       key[R2_KEY_MAX]; // delete: the share; upload: the name to reuse ("" = a new one)
    char       replace[R2_KEY_MAX]; // upload, free: the history entry this share takes the place of
    char       hosts[64]; // free: link_hosts
    i32        hours;     // free: litterbox_hours
    Path       file;    // probe: the file; upload: the copy to send (deleted after)
    Path       ffprobe;
    R2Conf     conf;
};

// One link job's own data (main thread, except `info`, which the probe fills under the lock).
struct R2Work {
    u32        save;  // the save_link job while preparing
    Path       src;
    MediaInfo  info;
    bool       has_info;
    EditRecord edit;
    char       title[256], game[64];
    u64        lib_key;
    char       reuse[R2_KEY_MAX]; // the removed share it brings back
};

static struct {
    SRWLOCK lock;
    R2Share shares[R2_MAX_SHARES]; // oldest first
    i32     count;
    bool    loaded;
    u32     gen;
    R2Job   jobs[16];
    R2Work  work[16];
    u32     next_id;
    R2View  test;
    i8      has_secret; // 0 unknown, 1 yes, -1 no
    R2Task  tasks[32];
    i32     task_count;
    HANDLE  thread, wake;
    volatile LONG stop;
} g_r2;

// ---------------------------------------------------------------------------------------------------------------
// Prices and limits

static i64 r2_limit_for_budget(f64 dollars)
{
    if (dollars < 0) dollars = 0;
    return (i64)((R2_FREE_GB + dollars / R2_PRICE_GB) * (f64)R2_GB);
}

static f64 r2_cost_for(i64 bytes)
{
    f64 gb = (f64)bytes / (f64)R2_GB - R2_FREE_GB;
    return gb > 0 ? gb * R2_PRICE_GB : 0;
}

// In the bucket now: an R2 share that cleanup hasn't removed.
static bool r2_in_bucket(const R2Share &s) { return s.host == HOST_R2 && !s.removed; }

// The link still plays: not removed and not past a free host's expiry.
static bool r2_share_live(const R2Share &s, i64 now)
{
    return !s.removed && (!s.expires_us || now < s.expires_us);
}

// Litterbox keeps files for 1, 12, 24 or 72 hours: the nearest of those at or above `hours`.
static i32 free_hours(i32 hours)
{
    static const i32 allowed[] = {1, 12, 24, 72};
    for (i32 h : allowed)
        if (hours <= h) return h;
    return 72;
}

static const FreeHost *free_host(LinkHost h)
{
    for (const FreeHost &f : FREE_HOSTS)
        if (f.host == h) return &f;
    return 0;
}

// The free hosts in link_hosts' order ("catbox,litterbox"), names it doesn't know skipped. Returns how many.
static i32 free_order(const char *hosts, const FreeHost **out, i32 cap)
{
    i32 n = 0;
    for (const char *p = hosts; *p && n < cap;) {
        const char *end = strchr(p, ',');
        i32 len = end ? (i32)(end - p) : (i32)strlen(p);
        for (const FreeHost &f : FREE_HOSTS) {
            bool dup = false;
            for (i32 k = 0; k < n; k++) dup |= out[k] == &f;
            if (!dup && (i32)strlen(f.name) == len && !_strnicmp(p, f.name, len)) out[n++] = &f;
        }
        p = end ? end + 1 : p + len;
    }
    return n;
}

// The biggest clip a free link takes with these hosts, 0 if none are on.
static i64 free_max_bytes(const char *hosts)
{
    const FreeHost *order[HOST_COUNT];
    i32 n = free_order(hosts, order, COUNT(order));
    i64 most = 0;
    for (i32 i = 0; i < n; i++) most = order[i]->max_bytes > most ? order[i]->max_bytes : most;
    return most;
}

// Which shares to delete so the bucket fits `limit` once `incoming` more bytes are in it: the oldest that aren't
// pinned, removed or at index `keep`. `incoming` also carries what's in the bucket's clipback/ folder that the
// history doesn't know of. Writes their indexes to out and returns how many, or -1 when the pinned shares, `keep`
// and `incoming` alone are over the limit.
static i32 r2_plan_cleanup(const R2Share *s, i32 n, i64 limit, i64 incoming, i32 keep, i32 *out, i32 cap)
{
    i64 used = incoming, fixed = incoming;
    for (i32 i = 0; i < n; i++) {
        if (!r2_in_bucket(s[i])) continue;
        used += s[i].bytes;
        if (s[i].pinned || i == keep) fixed += s[i].bytes;
    }
    if (fixed > limit) return -1;
    i32 count = 0;
    while (used > limit && count < cap) {
        i32 oldest = -1;
        for (i32 i = 0; i < n; i++) {
            if (!r2_in_bucket(s[i]) || s[i].pinned || i == keep) continue;
            bool taken = false;
            for (i32 k = 0; k < count; k++) taken |= out[k] == i;
            if (!taken && (oldest < 0 || s[i].uploaded_us < s[oldest].uploaded_us)) oldest = i;
        }
        if (oldest < 0) break;
        out[count++] = oldest;
        used -= s[oldest].bytes;
    }
    return count;
}

// ---------------------------------------------------------------------------------------------------------------
// Names and links

// `n` random letters and digits (16 is about 95 bits).
static void r2_random(char *out, i32 n)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    i32 have = 0;
    while (have < n) {
        u8 b[32];
        BCryptGenRandom(0, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        for (i32 i = 0; i < 32 && have < n; i++)
            if (b[i] < 248) out[have++] = abc[b[i] % 62]; // 248 = 4 * 62: no letter more likely than another
    }
    out[n] = 0;
}

// A new clip's name in the bucket: clipback/<16 random>/clip.mp4.
static void r2_new_key(char out[R2_KEY_MAX])
{
    char folder[17];
    r2_random(folder, 16);
    snprintf(out, R2_KEY_MAX, "clipback/%s/clip.mp4", folder);
}

static void r2_make_url(const char *public_url, const char *key, i32 version, char *out, i32 cap)
{
    i32 len = (i32)strlen(public_url);
    while (len > 0 && public_url[len - 1] == '/') len--;
    if (version > 1) snprintf(out, cap, "%.*s/%s?v=%d", len, public_url, key, version);
    else snprintf(out, cap, "%.*s/%s", len, public_url, key);
}

// ---------------------------------------------------------------------------------------------------------------
// The secret key, in Windows' Credential Manager. A test copy (CLIPBACK_HOME) keeps its own.

static void r2_cred_target(wchar_t *out, i32 cap)
{
    u32 h = test_home_hash();
    if (h) swprintf(out, cap, L"clipback-%08x/r2", h);
    else swprintf(out, cap, L"clipback/r2");
}

static bool r2_secret_get(char *out, i32 cap)
{
    out[0] = 0;
    wchar_t target[64];
    r2_cred_target(target, COUNT(target));
    CREDENTIALW *cred = 0;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &cred)) return false;
    i32 n = (i32)cred->CredentialBlobSize;
    if (n >= cap) n = cap - 1;
    memcpy(out, cred->CredentialBlob, n);
    out[n] = 0;
    CredFree(cred);
    return out[0] != 0;
}

// Stores the secret key; "" removes it.
static void r2_secret_set(const char *s)
{
    wchar_t target[64];
    r2_cred_target(target, COUNT(target));
    if (!s || !s[0]) {
        CredDeleteW(target, CRED_TYPE_GENERIC, 0);
        g_r2.has_secret = -1;
        return;
    }
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = target;
    cred.CredentialBlobSize = (DWORD)strlen(s);
    cred.CredentialBlob = (LPBYTE)s;
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = (wchar_t *)L"clipback";
    if (CredWriteW(&cred, 0)) g_r2.has_secret = 1;
    else log_line("r2: couldn't store the secret key (%d)", (int)GetLastError());
}

static bool r2_has_secret()
{
    if (!g_r2.has_secret) {
        char s[96];
        g_r2.has_secret = r2_secret_get(s, sizeof s) ? 1 : -1;
        SecureZeroMemory(s, sizeof s);
    }
    return g_r2.has_secret > 0;
}

static bool r2_ready()
{
    const Config &c = g_app.config;
    return c.r2_account[0] && c.r2_access_key[0] && c.r2_bucket[0] && c.r2_public_url[0] && r2_has_secret();
}

static bool r2_conf(R2Conf *out)
{
    const Config &c = g_app.config;
    *out = {};
    str_set(out->account, c.r2_account);
    str_set(out->access, c.r2_access_key);
    str_set(out->bucket, c.r2_bucket);
    str_set(out->public_url, c.r2_public_url);
    out->limit = r2_limit_for_budget(c.r2_budget);
    r2_secret_get(out->secret, sizeof out->secret);
    return out->account[0] && out->access[0] && out->secret[0] && out->bucket[0] && out->public_url[0];
}

// ---------------------------------------------------------------------------------------------------------------
// AWS Signature V4, which R2's S3 API takes (region "auto", service "s3").

static bool r2_sha256(const void *data, size_t n, u8 out[32], const void *key = 0, size_t key_len = 0)
{
    BCRYPT_ALG_HANDLE alg = 0;
    BCRYPT_HASH_HANDLE h = 0;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, 0,
                                                         key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, 0, 0, (PUCHAR)key, (ULONG)key_len, 0)) &&
              BCRYPT_SUCCESS(BCryptHashData(h, (PUCHAR)data, (ULONG)n, 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(h, out, 32, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static void r2_hex(const u8 *b, i32 n, char *out)
{
    for (i32 i = 0; i < n; i++) snprintf(out + i * 2, 3, "%02x", b[i]);
}

struct SigHeader {
    const char *name, *value; // name in lower case; given sorted by name
};

// The Authorization header's value for a request. `uri` and `query` are as sent (already encoded, the query's
// parameters sorted); `amz_date` is "YYYYMMDDTHHMMSSZ".
static void sigv4_auth(const char *method, const char *uri, const char *query, const SigHeader *h, i32 n,
                       const char *payload_hash, const char *access, const char *secret, const char *amz_date,
                       const char *region, const char *service, char *out, i32 cap)
{
    static thread_local char canon[4096], to_sign[1024];
    char signed_names[256] = "";
    i32 len = snprintf(canon, sizeof canon, "%s\n%s\n%s\n", method, uri, query);
    for (i32 i = 0; i < n; i++) {
        len += snprintf(canon + len, sizeof canon - len, "%s:%s\n", h[i].name, h[i].value);
        if (i) strncat(signed_names, ";", sizeof signed_names - strlen(signed_names) - 1);
        strncat(signed_names, h[i].name, sizeof signed_names - strlen(signed_names) - 1);
    }
    snprintf(canon + len, sizeof canon - len, "\n%s\n%s", signed_names, payload_hash);

    u8 hash[32];
    char hash_hex[65];
    r2_sha256(canon, strlen(canon), hash);
    r2_hex(hash, 32, hash_hex);
    char day[9], scope[128];
    snprintf(day, sizeof day, "%.8s", amz_date);
    snprintf(scope, sizeof scope, "%s/%s/%s/aws4_request", day, region, service);
    snprintf(to_sign, sizeof to_sign, "AWS4-HMAC-SHA256\n%s\n%s\n%s", amz_date, scope, hash_hex);

    char first[128];
    snprintf(first, sizeof first, "AWS4%s", secret);
    u8 k[32];
    r2_sha256(day, strlen(day), k, first, strlen(first));
    r2_sha256(region, strlen(region), k, k, 32);
    r2_sha256(service, strlen(service), k, k, 32);
    r2_sha256("aws4_request", 12, k, k, 32);
    u8 sig[32];
    char sig_hex[65];
    r2_sha256(to_sign, strlen(to_sign), sig, k, 32);
    r2_hex(sig, 32, sig_hex);
    SecureZeroMemory(first, sizeof first);
    SecureZeroMemory(k, sizeof k);
    snprintf(out, cap, "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s", access, scope,
             signed_names, sig_hex);
}

// Percent-encodes everything but A-Z a-z 0-9 - _ . ~ (S3's rule for query values).
static void r2_uri_encode(const char *s, char *out, i32 cap)
{
    i32 n = 0;
    for (; *s && n < cap - 4; s++) {
        u8 c = (u8)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = (char)c;
        else n += snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n] = 0;
}

// The text of the first <tag>...</tag> in an XML answer.
static bool r2_xml(const char *xml, const char *tag, char *out, i32 cap)
{
    out[0] = 0;
    char open[64], close[64];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *a = xml ? strstr(xml, open) : 0;
    const char *b = a ? strstr(a, close) : 0;
    if (!b) return false;
    a += strlen(open);
    snprintf(out, cap, "%.*s", (int)(b - a), a);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Requests to the bucket (worker thread)

struct R2Body {
    const char *mem; // or
    HANDLE      file;
    i64         offset, len;
};

struct R2Progress {
    u32 job;
    i64 done, total;
};

static void r2_set_progress(u32 job, f32 p)
{
    AcquireSRWLockExclusive(&g_r2.lock);
    for (i32 i = 0; i < COUNT(g_r2.jobs); i++)
        if (job && g_r2.jobs[i].id == job) g_r2.jobs[i].progress = p;
    ReleaseSRWLockExclusive(&g_r2.lock);
}

// One signed request to <account>.r2.cloudflarestorage.com/<bucket>/<key>?<query>. Returns the status (0 = no
// answer); the answer's body (cut at resp_cap) goes to resp and its ETag to etag.
static u32 r2_send(const R2Conf &c, const char *method, const char *key, const char *query, const R2Body &body,
                   const char *content_type, char *resp, i32 resp_cap, char *etag = 0, i32 etag_cap = 0,
                   R2Progress *prog = 0)
{
    if (resp_cap) resp[0] = 0;
    if (etag_cap) etag[0] = 0;
    char host[128], uri[256], date[20], auth[512], hdrs[1024];
    snprintf(host, sizeof host, "%s.r2.cloudflarestorage.com", c.account);
    snprintf(uri, sizeof uri, "/%s/%s", c.bucket, key);
    SYSTEMTIME t;
    GetSystemTime(&t);
    snprintf(date, sizeof date, "%04d%02d%02dT%02d%02d%02dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    SigHeader h[] = {{"host", host}, {"x-amz-content-sha256", "UNSIGNED-PAYLOAD"}, {"x-amz-date", date}};
    sigv4_auth(method, uri, query, h, COUNT(h), "UNSIGNED-PAYLOAD", c.access, c.secret, date, "auto", "s3", auth,
               sizeof auth);
    snprintf(hdrs, sizeof hdrs, "x-amz-content-sha256: UNSIGNED-PAYLOAD\r\nx-amz-date: %s\r\nAuthorization: %s%s%s",
             date, auth, content_type ? "\r\nContent-Type: " : "", content_type ? content_type : "");

    wchar_t whost[128], wmethod[16], wpath[1024], whdrs[1024];
    char path[1024];
    snprintf(path, sizeof path, "%s%s%s", uri, query[0] ? "?" : "", query);
    utf8_to_wide(host, whost, COUNT(whost));
    utf8_to_wide(method, wmethod, COUNT(wmethod));
    utf8_to_wide(path, wpath, COUNT(wpath));
    utf8_to_wide(hdrs, whdrs, COUNT(whdrs));

    u32 status = 0;
    HINTERNET session = WinHttpOpen(L"clipback", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0);
    HINTERNET conn = session ? WinHttpConnect(session, whost, INTERNET_DEFAULT_HTTPS_PORT, 0) : 0;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, wmethod, wpath, 0, 0, 0, WINHTTP_FLAG_SECURE) : 0;
    if (req) {
        WinHttpSetTimeouts(req, 15000, 15000, 60000, 120000);
        DWORD total = (DWORD)body.len;
        bool sent = WinHttpSendRequest(req, whdrs, (DWORD)-1L, body.mem ? (void *)body.mem : 0,
                                       body.mem ? total : 0, total, 0) != 0;
        if (sent && body.file) {
            static thread_local u8 *buf;
            if (!buf) buf = (u8 *)malloc(1 << 20);
            LARGE_INTEGER at;
            at.QuadPart = body.offset;
            sent = SetFilePointerEx(body.file, at, 0, FILE_BEGIN) != 0;
            i64 left = body.len;
            while (sent && left > 0) {
                DWORD want = (DWORD)(left < (1 << 20) ? left : (1 << 20)), got = 0, wrote = 0;
                if (!ReadFile(body.file, buf, want, &got, 0) || got != want) { sent = false; break; }
                if (!WinHttpWriteData(req, buf, got, &wrote) || wrote != got) { sent = false; break; }
                left -= got;
                if (prog) {
                    prog->done += got;
                    r2_set_progress(prog->job, (f32)((f64)prog->done / (f64)prog->total));
                }
                if (g_r2.stop) sent = false;
            }
        }
        DWORD code = 0, len = sizeof code;
        if (sent && WinHttpReceiveResponse(req, 0) &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &code, &len, 0))
            status = code;
        if (status && etag_cap) {
            wchar_t w[128];
            DWORD wlen = sizeof w;
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_ETAG, 0, w, &wlen, 0)) wide_to_utf8(w, etag, etag_cap);
        }
        i32 have = 0;
        while (status && resp_cap) {
            char chunk[4096];
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk, sizeof chunk, &got) || !got) break;
            i32 take = (i32)got < resp_cap - 1 - have ? (i32)got : resp_cap - 1 - have;
            memcpy(resp + have, chunk, take);
            have += take;
            resp[have] = 0;
        }
        if (!status) DEV_LOG("r2: %s %s: no answer (error %d)", method, path, (int)GetLastError());
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    return status;
}

// Why the bucket said no, for the player: R2's error code, or the status.
static void r2_why(u32 status, const char *resp, const R2Conf &c, char *out, i32 cap)
{
    char code[64];
    r2_xml(resp, "Code", code, sizeof code);
    if (!status) snprintf(out, cap, "Couldn't reach %s.r2.cloudflarestorage.com: check the account ID", c.account);
    else if (!strcmp(code, "NoSuchBucket")) snprintf(out, cap, "There's no bucket named %s", c.bucket);
    else if (!strcmp(code, "InvalidAccessKeyId") || !strcmp(code, "SignatureDoesNotMatch"))
        snprintf(out, cap, "The bucket didn't take the keys: check the access key and secret");
    else if (status == 401 || status == 403)
        snprintf(out, cap, "The keys can't write to %s: give the token Object Read & Write", c.bucket);
    else snprintf(out, cap, "The bucket answered %u%s%s", status, code[0] ? " " : "", code);
}

static bool r2_ok(u32 status) { return status >= 200 && status < 300; }

// The total size of everything in the bucket's clipback/ folder (ListObjectsV2), -1 if it couldn't be listed.
static i64 r2_folder_bytes(const R2Conf &c)
{
    const i32 cap = 1 << 20;
    char *resp = (char *)malloc(cap);
    char token[1024] = "";
    i64 total = 0;
    for (i32 page = 0; page < 1000 && total >= 0; page++) {
        char query[2200], enc[2048];
        r2_uri_encode(token, enc, sizeof enc);
        if (token[0]) snprintf(query, sizeof query, "continuation-token=%s&list-type=2&prefix=clipback%%2F", enc);
        else snprintf(query, sizeof query, "list-type=2&prefix=clipback%%2F");
        u32 status = r2_send(c, "GET", "", query, R2Body{}, 0, resp, cap);
        if (!r2_ok(status)) {
            log_line("r2: couldn't list the bucket (%u)", status);
            total = -1;
            break;
        }
        for (const char *p = strstr(resp, "<Size>"); p; p = strstr(p + 6, "<Size>")) total += _atoi64(p + 6);
        if (!strstr(resp, "<IsTruncated>true</IsTruncated>") ||
            !r2_xml(resp, "NextContinuationToken", token, sizeof token))
            break;
    }
    free(resp);
    return total;
}

// What's in the bucket's clipback/ folder that the history doesn't count (under the lock): `folder` less the live
// shares, 0 when the folder couldn't be listed.
static i64 r2_untracked_locked(i64 folder)
{
    if (folder < 0) return 0;
    for (i32 i = 0; i < g_r2.count; i++)
        if (r2_in_bucket(g_r2.shares[i])) folder -= g_r2.shares[i].bytes;
    return folder > 0 ? folder : 0;
}

static bool r2_delete_object(const R2Conf &c, const char *key)
{
    char resp[1024];
    u32 status = r2_send(c, "DELETE", key, "", R2Body{}, 0, resp, sizeof resp);
    if (!r2_ok(status) && status != 404) log_line("r2: couldn't delete %s (%u)", key, status);
    return r2_ok(status) || status == 404;
}

// Sends a file to `key`: in one request, or in parts when large. On failure, says why in err.
static bool r2_put_file(const R2Conf &c, const char *key, const Path &file, u32 job, char *err, i32 cap)
{
    err[0] = 0;
    HANDLE f = CreateFileW(file.s, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(err, cap, "Couldn't open the copy to upload");
        return false;
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    R2Progress prog = {job, 0, size.QuadPart > 0 ? size.QuadPart : 1};
    static thread_local char resp[8192];
    bool ok = false;
    if (size.QuadPart <= R2_SINGLE_MAX) {
        u32 status = r2_send(c, "PUT", key, "", R2Body{0, f, 0, size.QuadPart}, "video/mp4", resp, sizeof resp, 0, 0,
                             &prog);
        ok = r2_ok(status);
        if (!ok) r2_why(status, resp, c, err, cap);
    } else {
        u32 status = r2_send(c, "POST", key, "uploads=", R2Body{}, "video/mp4", resp, sizeof resp);
        char id[512], id_enc[1024];
        if (!r2_ok(status) || !r2_xml(resp, "UploadId", id, sizeof id)) {
            r2_why(status, resp, c, err, cap);
            CloseHandle(f);
            return false;
        }
        r2_uri_encode(id, id_enc, sizeof id_enc);
        i32 parts = (i32)((size.QuadPart + R2_PART - 1) / R2_PART);
        i64 done_cap = (i64)parts * 128 + 256;
        char *done = (char *)malloc((size_t)done_cap);
        i32 len = snprintf(done, (size_t)done_cap, "<CompleteMultipartUpload>");
        ok = true;
        for (i32 p = 1; p <= parts && ok; p++) {
            i64 at = (i64)(p - 1) * R2_PART, n = size.QuadPart - at < R2_PART ? size.QuadPart - at : R2_PART;
            char query[1200], tag[128];
            snprintf(query, sizeof query, "partNumber=%d&uploadId=%s", p, id_enc);
            i64 before = prog.done;
            status = 0;
            for (i32 attempt = 0; attempt < 3 && !r2_ok(status) && !g_r2.stop; attempt++) {
                prog.done = before;
                status = r2_send(c, "PUT", key, query, R2Body{0, f, at, n}, 0, resp, sizeof resp, tag, sizeof tag,
                                 &prog);
            }
            ok = r2_ok(status) && tag[0];
            if (!ok) r2_why(status, resp, c, err, cap);
            else len += snprintf(done + len, (size_t)(done_cap - len),
                                 "<Part><PartNumber>%d</PartNumber><ETag>%s</ETag></Part>", p, tag);
        }
        snprintf(done + len, (size_t)(done_cap - len), "</CompleteMultipartUpload>");
        char query[1100];
        snprintf(query, sizeof query, "uploadId=%s", id_enc);
        if (ok) {
            status = r2_send(c, "POST", key, query, R2Body{done, 0, 0, (i64)strlen(done)}, "application/xml", resp,
                             sizeof resp);
            // A 200 can still carry an error for a complete.
            ok = r2_ok(status) && !strstr(resp, "<Error>");
            if (!ok) r2_why(status, resp, c, err, cap);
        }
        if (!ok) r2_send(c, "DELETE", key, query, R2Body{}, 0, resp, sizeof resp); // abort: drop the parts
        free(done);
    }
    CloseHandle(f);
    if (!ok && g_r2.stop) snprintf(err, cap, "Stopped");
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// History: <config_dir>\shares.tsv, one share a line, oldest first:
//   key  version  pinned  removed  bytes  uploaded_us  lib_key  host  expires_us  url  title  game  path

static void r2_tsv_field(FILE *f, const char *s, bool last)
{
    for (; *s; s++) fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f);
    fputc(last ? '\n' : '\t', f);
}

static bool r2_history_write(const Path &file, const R2Share *s, i32 n)
{
    Path part = file;
    wcsncat(part.s, L".part", MAX_PATH - wcslen(part.s) - 1);
    FILE *f = _wfopen(part.s, L"wb");
    if (!f) return false;
    for (i32 i = 0; i < n; i++) {
        fprintf(f, "%s\t%d\t%d\t%d\t%lld\t%lld\t%llu\t%d\t%lld\t", s[i].key, s[i].version, s[i].pinned ? 1 : 0,
                s[i].removed ? 1 : 0, s[i].bytes, s[i].uploaded_us, s[i].lib_key, (int)s[i].host, s[i].expires_us);
        r2_tsv_field(f, s[i].url, false);
        r2_tsv_field(f, s[i].title, false);
        r2_tsv_field(f, s[i].game, false);
        r2_tsv_field(f, s[i].path, true);
    }
    bool ok = fclose(f) == 0;
    return ok && MoveFileExW(part.s, file.s, MOVEFILE_REPLACE_EXISTING);
}

static i32 r2_history_read(const Path &file, R2Share *out, i32 max)
{
    char *text = read_file(file.s, 0);
    if (!text) return 0;
    i32 n = 0;
    for (char *line = text; line && *line && n < max;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *f[13] = {};
        i32 count = 0;
        for (char *p = line; count < 13;) {
            f[count++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }
        if (count == 13 && strlen(f[0]) < R2_KEY_MAX && atoi(f[7]) >= 0 && atoi(f[7]) < HOST_COUNT) {
            R2Share &s = out[n++];
            s = {};
            str_set(s.key, f[0]);
            s.version = atoi(f[1]);
            s.pinned = f[2][0] == '1';
            s.removed = f[3][0] == '1';
            s.bytes = _atoi64(f[4]);
            s.uploaded_us = _atoi64(f[5]);
            s.lib_key = _strtoui64(f[6], 0, 10);
            s.host = (LinkHost)atoi(f[7]);
            s.expires_us = _atoi64(f[8]);
            str_set(s.url, f[9]);
            str_set(s.title, f[10]);
            str_set(s.game, f[11]);
            str_set(s.path, f[12]);
        }
        line = next;
    }
    free(text);
    return n;
}

static Path r2_history_file() { return path_join(g_app.paths.config_dir, L"shares.tsv"); }

// Under the lock.
static void r2_load_locked()
{
    if (g_r2.loaded) return;
    g_r2.loaded = true;
    g_r2.count = r2_history_read(r2_history_file(), g_r2.shares, R2_MAX_SHARES);
}

static void r2_save_locked()
{
    g_r2.gen++;
    if (!r2_history_write(r2_history_file(), g_r2.shares, g_r2.count)) log_line("r2: couldn't write shares.tsv");
}

static i32 r2_find_locked(const char *key)
{
    for (i32 i = 0; i < g_r2.count; i++)
        if (!strcmp(g_r2.shares[i].key, key)) return i;
    return -1;
}

static void r2_drop_locked(i32 i)
{
    g_r2.count--;
    memmove(g_r2.shares + i, g_r2.shares + i + 1, (g_r2.count - i) * sizeof g_r2.shares[0]);
}

// Drops the entry `key` names, if any (the share a new one takes the place of).
static void r2_replace_locked(const char *key)
{
    i32 i = key[0] ? r2_find_locked(key) : -1;
    if (i >= 0) r2_drop_locked(i);
}

// A full history makes room for one more: the oldest entry whose link no longer plays, else the oldest.
static void r2_room_locked()
{
    if (g_r2.count < R2_MAX_SHARES) return;
    i32 drop = 0;
    i64 now = now_us();
    for (i32 i = 0; i < g_r2.count; i++)
        if (!r2_share_live(g_r2.shares[i], now)) { drop = i; break; }
    r2_drop_locked(drop);
}

// ---------------------------------------------------------------------------------------------------------------
// The worker

static void r2_post(R2EventKind kind, i64 b)
{
    Event e = {};
    e.kind = EV_R2;
    e.a = kind;
    e.b = b;
    event_post(e);
}

static void r2_job_end(u32 id, bool ok, const char *url, const char *error)
{
    AcquireSRWLockExclusive(&g_r2.lock);
    for (i32 i = 0; i < COUNT(g_r2.jobs); i++) {
        R2Job &j = g_r2.jobs[i];
        if (j.id != id) continue;
        j.state = ok ? R2_DONE : R2_FAILED;
        j.progress = 1;
        str_set(j.url, url);
        str_set(j.error, error);
    }
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (!ok) log_line("r2: job %u failed: %s", id, error);
    else DEV_LOG("r2: job %u: %s", id, url);
    r2_post(R2_EV_JOB, id);
}

static void r2_test_end(bool ok, const char *text)
{
    AcquireSRWLockExclusive(&g_r2.lock);
    g_r2.test.state = ok ? R2_TEST_OK : R2_TEST_FAILED;
    str_set(g_r2.test.text, text);
    ReleaseSRWLockExclusive(&g_r2.lock);
    r2_post(R2_EV_TEST, ok);
}

static void r2_run_test(const R2Conf &c)
{
    char key[17], body[64], resp[2048], why[256];
    r2_random(key, 16);
    snprintf(body, sizeof body, "clipback %s", key);
    char name[64];
    snprintf(name, sizeof name, "clipback/test-%s.txt", key);
    u32 status = r2_send(c, "PUT", name, "", R2Body{body, 0, 0, (i64)strlen(body)}, "text/plain", resp, sizeof resp);
    if (!r2_ok(status)) {
        r2_why(status, resp, c, why, sizeof why);
        r2_test_end(false, why);
        return;
    }
    // Fetched the way a viewer would
    char url[512], fail[256] = "";
    r2_make_url(c.public_url, name, 1, url, sizeof url);
    wchar_t wurl[512], whost[256] = {}, wpath[512] = {};
    utf8_to_wide(url, wurl, COUNT(wurl));
    URL_COMPONENTSW parts = {};
    parts.dwStructSize = sizeof parts;
    parts.lpszHostName = whost;
    parts.dwHostNameLength = COUNT(whost);
    parts.lpszUrlPath = wpath;
    parts.dwUrlPathLength = COUNT(wpath);
    if (!starts_with(c.public_url, "https://") || !WinHttpCrackUrl(wurl, 0, 0, &parts)) {
        snprintf(fail, sizeof fail, "The public address should start with https://");
    } else {
        char *got = 0;
        u32 answer = http_get(whost, wpath, L"", 0, &got, 0);
        if (answer != 200 || !got || strcmp(got, body))
            snprintf(fail, sizeof fail,
                     answer ? "Uploaded, but %ls answered %u: turn on public access for the bucket"
                            : "Uploaded, but %ls didn't answer: check the public address",
                     whost, answer);
        free(got);
    }
    bool deleted = r2_delete_object(c, name);
    if (fail[0]) r2_test_end(false, fail);
    else if (!deleted) r2_test_end(false, "Works, but the keys can't delete: give the token Object Read & Write");
    else {
        snprintf(why, sizeof why, "Works: uploaded, played from %ls and deleted", whost);
        r2_test_end(true, why);
    }
}

// The upload step of a job, then the cleanup.
static void r2_run_upload(const R2Task &t)
{
    u32 job = 0;
    R2Work w = {};
    AcquireSRWLockExclusive(&g_r2.lock);
    job = g_r2.jobs[t.slot].id;
    g_r2.jobs[t.slot].state = R2_UPLOADING;
    g_r2.jobs[t.slot].progress = 0;
    str_set(w.title, g_r2.work[t.slot].title);
    str_set(w.game, g_r2.work[t.slot].game);
    w.src = g_r2.work[t.slot].src;
    w.lib_key = g_r2.work[t.slot].lib_key;
    ReleaseSRWLockExclusive(&g_r2.lock);

    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    i64 bytes = GetFileAttributesExW(t.file.s, GetFileExInfoStandard, &fa)
                    ? ((i64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow : 0;
    char key[R2_KEY_MAX], err[256] = "";
    i32 plan[R2_MAX_SHARES];

    // Room first: the pinned clips must leave space for this one.
    i64 folder = r2_folder_bytes(t.conf);
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    i32 reuse = t.key[0] ? r2_find_locked(t.key) : -1;
    i64 untracked = r2_untracked_locked(folder);
    if (r2_plan_cleanup(g_r2.shares, g_r2.count, t.conf.limit, bytes + untracked, -1, plan, COUNT(plan)) < 0)
        str_set(err, bytes > t.conf.limit ? "The clip is bigger than your limit: raise it in Options"
                                          : "Pinned clips fill your limit: unpin some or raise it");
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (reuse >= 0) str_set(key, t.key);
    else r2_new_key(key);

    if (!err[0]) r2_put_file(t.conf, key, t.file, job, err, sizeof err);
    if (!err[0]) folder = r2_folder_bytes(t.conf); // the real total, with this clip in it
    DeleteFileW(t.file.s);
    if (err[0]) {
        r2_job_end(job, false, "", err);
        return;
    }

    char url[512];
    AcquireSRWLockExclusive(&g_r2.lock);
    if (strcmp(t.replace, key)) r2_replace_locked(t.replace);
    i32 at = r2_find_locked(key);
    if (at < 0) {
        r2_room_locked();
        at = g_r2.count++;
        g_r2.shares[at] = {};
        str_set(g_r2.shares[at].key, key);
    } else { // shared again: moves to the end, as the newest
        R2Share s = g_r2.shares[at];
        r2_drop_locked(at);
        at = g_r2.count++;
        g_r2.shares[at] = s;
    }
    R2Share &s = g_r2.shares[at];
    s.version++;
    s.removed = false;
    s.host = HOST_R2;
    s.expires_us = 0;
    s.bytes = bytes;
    s.uploaded_us = now_us();
    s.lib_key = w.lib_key;
    str_set(s.title, w.title);
    str_set(s.game, w.game);
    wide_to_utf8(w.src.s, s.path, sizeof s.path);
    r2_make_url(t.conf.public_url, key, s.version, s.url, sizeof s.url);
    str_set(url, s.url);
    i32 n = r2_plan_cleanup(g_r2.shares, g_r2.count, t.conf.limit, r2_untracked_locked(folder), at, plan,
                            COUNT(plan));
    static char gone[R2_MAX_SHARES][R2_KEY_MAX];
    for (i32 i = 0; i < n; i++) str_set(gone[i], g_r2.shares[plan[i]].key);
    r2_save_locked();
    ReleaseSRWLockExclusive(&g_r2.lock);
    r2_job_end(job, true, url, "");

    // Over the limit: the oldest unpinned clips leave the bucket.
    for (i32 i = 0; i < n; i++) {
        if (!r2_delete_object(t.conf, gone[i])) continue;
        AcquireSRWLockExclusive(&g_r2.lock);
        i32 k = r2_find_locked(gone[i]);
        if (k >= 0) g_r2.shares[k].removed = true;
        r2_save_locked();
        ReleaseSRWLockExclusive(&g_r2.lock);
        log_line("r2: removed %s to stay under the limit", gone[i]);
    }
    if (n > 0) r2_post(R2_EV_HISTORY, 0);
}

// A form upload of `f` as fileToUpload ("clip.mp4", video/mp4) after `fields` (name, value pairs), read back into
// resp. Returns the status, 0 if the host couldn't be reached.
static u32 free_post(const FreeHost &h, const char *const *fields, i32 nfields, HANDLE f, i64 size,
                     R2Progress *prog, char *resp, i32 cap)
{
    resp[0] = 0;
    char boundary[48], pre[2048], post[64];
    char rnd[25];
    r2_random(rnd, 24);
    snprintf(boundary, sizeof boundary, "clipback%s", rnd);
    i32 len = 0;
    for (i32 i = 0; i + 1 < nfields; i += 2)
        len += snprintf(pre + len, sizeof pre - len,
                        "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", boundary, fields[i],
                        fields[i + 1]);
    len += snprintf(pre + len, sizeof pre - len,
                    "--%s\r\nContent-Disposition: form-data; name=\"fileToUpload\"; filename=\"clip.mp4\"\r\n"
                    "Content-Type: video/mp4\r\n\r\n",
                    boundary);
    i32 post_len = snprintf(post, sizeof post, "\r\n--%s--\r\n", boundary);
    wchar_t hdrs[160];
    swprintf(hdrs, COUNT(hdrs), L"Content-Type: multipart/form-data; boundary=%hs", boundary);

    u32 status = 0;
    HINTERNET session = WinHttpOpen(L"clipback", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0);
    HINTERNET conn = session ? WinHttpConnect(session, h.server, INTERNET_DEFAULT_HTTPS_PORT, 0) : 0;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"POST", h.path, 0, 0, 0, WINHTTP_FLAG_SECURE) : 0;
    if (req) {
        WinHttpSetTimeouts(req, 15000, 15000, 120000, 300000); // the host answers once the whole file is in
        DWORD total = (DWORD)(len + size + post_len), wrote = 0;
        LARGE_INTEGER zero = {};
        bool sent = SetFilePointerEx(f, zero, 0, FILE_BEGIN) &&
                    WinHttpSendRequest(req, hdrs, (DWORD)-1L, 0, 0, total, 0) &&
                    WinHttpWriteData(req, pre, len, &wrote) && wrote == (DWORD)len;
        static thread_local u8 *buf;
        if (!buf) buf = (u8 *)malloc(1 << 20);
        for (i64 left = size; sent && left > 0;) {
            DWORD want = (DWORD)(left < (1 << 20) ? left : (1 << 20)), got = 0;
            if (!ReadFile(f, buf, want, &got, 0) || got != want) { sent = false; break; }
            if (!WinHttpWriteData(req, buf, got, &wrote) || wrote != got) { sent = false; break; }
            left -= got;
            prog->done += got;
            r2_set_progress(prog->job, (f32)((f64)prog->done / (f64)prog->total));
            if (g_r2.stop) sent = false;
        }
        sent = sent && WinHttpWriteData(req, post, post_len, &wrote) && wrote == (DWORD)post_len;
        DWORD code = 0, clen = sizeof code;
        if (sent && WinHttpReceiveResponse(req, 0) &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &code, &clen, 0))
            status = code;
        i32 have = 0;
        while (status) {
            DWORD got = 0;
            if (have >= cap - 1 || !WinHttpReadData(req, resp + have, cap - 1 - have, &got) || !got) break;
            have += (i32)got;
            resp[have] = 0;
        }
        if (!status) DEV_LOG("r2: %hs: no answer (error %d)", h.name, (int)GetLastError());
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    return status;
}

// The upload step of a job with no R2: the free hosts in the player's order, each that takes the size, until one
// gives a link.
static void r2_run_free(const R2Task &t)
{
    u32 job = 0;
    R2Work w = {};
    AcquireSRWLockExclusive(&g_r2.lock);
    job = g_r2.jobs[t.slot].id;
    g_r2.jobs[t.slot].state = R2_UPLOADING;
    g_r2.jobs[t.slot].progress = 0;
    str_set(w.title, g_r2.work[t.slot].title);
    str_set(w.game, g_r2.work[t.slot].game);
    w.src = g_r2.work[t.slot].src;
    w.lib_key = g_r2.work[t.slot].lib_key;
    ReleaseSRWLockExclusive(&g_r2.lock);

    char err[256] = "", url[512] = "";
    const FreeHost *order[HOST_COUNT], *used = 0;
    i32 n = free_order(t.hosts, order, COUNT(order));
    HANDLE f = CreateFileW(t.file.s, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    LARGE_INTEGER size = {};
    if (f == INVALID_HANDLE_VALUE) str_set(err, "Couldn't open the copy to upload");
    else GetFileSizeEx(f, &size);
    bool tried = false;
    char hours[8];
    snprintf(hours, sizeof hours, "%dh", free_hours(t.hours));
    for (i32 i = 0; i < n && !err[0] && !url[0] && !g_r2.stop; i++) {
        const FreeHost &h = *order[i];
        if (size.QuadPart > h.max_bytes) continue;
        tried = true;
        const char *fields[] = {"reqtype", "fileupload", "time", hours};
        R2Progress prog = {job, 0, size.QuadPart > 0 ? size.QuadPart : 1};
        r2_set_progress(job, 0);
        static thread_local char resp[4096];
        u32 status = free_post(h, fields, h.expires ? 4 : 2, f, size.QuadPart, &prog, resp, sizeof resp);
        char *r = trim(resp);
        if (status == 200 && starts_with(r, "https://") && !strchr(r, ' ') && strlen(r) < sizeof url) {
            str_set(url, r);
            used = &h;
        } else {
            if (!status) snprintf(err, sizeof err, "Couldn't reach %s", h.label);
            else snprintf(err, sizeof err, "%s answered %u: %.80s", h.label, status, r);
            log_line("r2: %s", err);
            if (i + 1 < n) err[0] = 0; // the next host gets the clip
        }
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    DeleteFileW(t.file.s);
    if (!url[0] && !err[0]) {
        i64 most = free_max_bytes(t.hosts);
        if (!n) str_set(err, "Turn on a free host or set up Cloudflare in Options");
        else if (!tried) snprintf(err, sizeof err, "Free links take clips up to %lld MB: set up Cloudflare in Options for bigger ones", most / 1000000);
        else str_set(err, g_r2.stop ? "Stopped" : "No free host took the clip");
    }
    if (!url[0]) {
        r2_job_end(job, false, "", err);
        return;
    }

    const char *name = strrchr(url, '/');
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    r2_replace_locked(t.replace);
    r2_room_locked();
    R2Share &s = g_r2.shares[g_r2.count++];
    s = {};
    snprintf(s.key, sizeof s.key, "%s/%.*s", used->name, (int)(sizeof s.key - strlen(used->name) - 2), name + 1);
    s.host = used->host;
    s.version = 1;
    s.bytes = size.QuadPart;
    s.uploaded_us = now_us();
    s.expires_us = used->expires ? s.uploaded_us + (i64)free_hours(t.hours) * 3600 * 1000000 : 0;
    s.lib_key = w.lib_key;
    str_set(s.url, url);
    str_set(s.title, w.title);
    str_set(s.game, w.game);
    wide_to_utf8(w.src.s, s.path, sizeof s.path);
    r2_save_locked();
    ReleaseSRWLockExclusive(&g_r2.lock);
    r2_job_end(job, true, url, "");
    r2_post(R2_EV_HISTORY, 0);
}

static void r2_run_delete(const R2Task &t)
{
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    i32 i = r2_find_locked(t.key);
    bool remote = i >= 0 && r2_in_bucket(g_r2.shares[i]);
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (i < 0) return;
    if (remote && !r2_delete_object(t.conf, t.key)) {
        event_post_text(EV_LOG, "Couldn't delete the clip from the bucket");
        return;
    }
    AcquireSRWLockExclusive(&g_r2.lock);
    i = r2_find_locked(t.key);
    if (i >= 0) r2_drop_locked(i);
    r2_save_locked();
    ReleaseSRWLockExclusive(&g_r2.lock);
    r2_post(R2_EV_HISTORY, 0);
}

static bool lib_probe(const Path &ffprobe, const Path &file, MediaInfo *m, char *tagged, i32 tagged_cap);

static DWORD WINAPI r2_thread(void *)
{
    while (!g_r2.stop) {
        static R2Task t;
        bool have = false;
        AcquireSRWLockExclusive(&g_r2.lock);
        if (g_r2.task_count) {
            t = g_r2.tasks[0];
            g_r2.task_count--;
            memmove(g_r2.tasks, g_r2.tasks + 1, g_r2.task_count * sizeof g_r2.tasks[0]);
            have = true;
        }
        ReleaseSRWLockExclusive(&g_r2.lock);
        if (!have) {
            WaitForSingleObject(g_r2.wake, INFINITE);
            continue;
        }
        switch (t.kind) {
        case R2_T_PROBE: {
            MediaInfo m;
            char tagged[256];
            bool ok = lib_probe(t.ffprobe, t.file, &m, tagged, sizeof tagged);
            AcquireSRWLockExclusive(&g_r2.lock);
            g_r2.work[t.slot].info = m;
            g_r2.work[t.slot].has_info = ok;
            ReleaseSRWLockExclusive(&g_r2.lock);
            r2_post(R2_EV_PROBED, t.slot);
        } break;
        case R2_T_UPLOAD: r2_run_upload(t); break;
        case R2_T_FREE: r2_run_free(t); break;
        case R2_T_DELETE: r2_run_delete(t); break;
        case R2_T_TEST: r2_run_test(t.conf); break;
        }
        SecureZeroMemory(t.conf.secret, sizeof t.conf.secret);
    }
    return 0;
}

static bool r2_queue(const R2Task &t)
{
    if (!g_r2.thread) {
        g_r2.wake = CreateEventW(0, FALSE, FALSE, 0);
        g_r2.thread = thread_start(r2_thread, 0);
    }
    AcquireSRWLockExclusive(&g_r2.lock);
    bool ok = g_r2.task_count < COUNT(g_r2.tasks);
    if (ok) g_r2.tasks[g_r2.task_count++] = t;
    ReleaseSRWLockExclusive(&g_r2.lock);
    SetEvent(g_r2.wake);
    return ok;
}

static void r2_stop()
{
    if (!g_r2.thread) return;
    InterlockedExchange(&g_r2.stop, 1);
    SetEvent(g_r2.wake);
    WaitForSingleObject(g_r2.thread, 3000);
}

// ---------------------------------------------------------------------------------------------------------------
// Main thread

// A slot for a new job: a free one, else the oldest finished. -1 if all are busy.
static i32 r2_new_job()
{
    i32 slot = -1;
    AcquireSRWLockExclusive(&g_r2.lock);
    for (i32 i = 0; i < COUNT(g_r2.jobs); i++) {
        R2Job &j = g_r2.jobs[i];
        if (!j.id) { slot = i; break; }
        if (j.state >= R2_DONE && (slot < 0 || j.id < g_r2.jobs[slot].id)) slot = i;
    }
    if (slot >= 0) {
        g_r2.jobs[slot] = {};
        g_r2.jobs[slot].id = ++g_r2.next_id;
        g_r2.jobs[slot].state = R2_QUEUED;
        g_r2.work[slot] = {};
    }
    ReleaseSRWLockExclusive(&g_r2.lock);
    return slot;
}

static u32 r2_fail_now(i32 slot, const char *error)
{
    u32 id = g_r2.jobs[slot].id;
    r2_job_end(id, false, "", error);
    return id;
}

// Starts the copy for a job whose file has been probed.
static void r2_start_copy(i32 slot)
{
    R2Work &w = g_r2.work[slot];
    char stem[256];
    if (w.title[0]) str_set(stem, w.title);
    else {
        const wchar_t *name = wcsrchr(w.src.s, L'\\');
        wide_to_utf8(name ? name + 1 : w.src.s, stem, sizeof stem);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
    }
    AcquireSRWLockExclusive(&g_r2.lock);
    g_r2.jobs[slot].state = R2_PREPARING;
    ReleaseSRWLockExclusive(&g_r2.lock);
    w.save = save_link(w.src, w.info, w.edit, stem);
    if (!w.save) r2_fail_now(slot, "Too many saves are waiting");
}

// Shares a file as a link, to R2 when it's set up, else to a free host: `info` and `edit` when the caller has them
// (an editor render passes its edit; with no info the file is probed first). A library item already shared gives
// back its link at once while that still plays; one removed to make room is shared again under its old name, one a
// free host dropped gets a new link. Follow the job with r2_job(id); EV_R2 says when it moves on. Returns the job
// id, 0 if every job slot is busy.
static u32 r2_share_file(const wchar_t *path, const char *title, const char *game, u64 lib_key,
                         const MediaInfo *info = 0, const EditRecord *edit = 0)
{
    i32 slot = r2_new_job();
    if (slot < 0) return 0;
    R2Work &w = g_r2.work[slot];
    if (lib_key && !edit) {
        AcquireSRWLockExclusive(&g_r2.lock);
        r2_load_locked();
        i32 found = -1;
        for (i32 i = g_r2.count - 1; i >= 0 && found < 0; i--)
            if (g_r2.shares[i].lib_key == lib_key) found = i;
        char url[512] = "";
        if (found >= 0 && r2_share_live(g_r2.shares[found], now_us())) str_set(url, g_r2.shares[found].url);
        else if (found >= 0) str_set(w.reuse, g_r2.shares[found].key);
        ReleaseSRWLockExclusive(&g_r2.lock);
        if (url[0]) {
            u32 id = g_r2.jobs[slot].id;
            r2_job_end(id, true, url, "");
            return id;
        }
    }
    wcsncpy(w.src.s, path, MAX_PATH - 1);
    str_set(w.title, title);
    str_set(w.game, game);
    w.lib_key = lib_key;
    if (edit) w.edit = *edit;
    if (GetFileAttributesW(w.src.s) == INVALID_FILE_ATTRIBUTES) return r2_fail_now(slot, "The file isn't there any more");
    AcquireSRWLockExclusive(&g_r2.lock);
    g_r2.jobs[slot].state = R2_PREPARING;
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (info) {
        w.info = *info;
        w.has_info = true;
        r2_start_copy(slot);
    } else {
        R2Task t = {};
        t.kind = R2_T_PROBE;
        t.slot = slot;
        t.file = w.src;
        Path ffmpeg = {};
        if (!ffmpeg_find(g_app.config.ffmpeg, &ffmpeg)) return r2_fail_now(slot, "ffmpeg wasn't found");
        t.ffprobe = ffprobe_for(ffmpeg);
        if (!r2_queue(t)) return r2_fail_now(slot, "Too many links are waiting");
    }
    return g_r2.jobs[slot].id;
}

// Shares a clip again whose link no longer plays: under the same name when it was in R2, else as a new link.
static u32 r2_reshare(const char *key)
{
    char path[1040] = "", title[256] = "", game[64] = "";
    u64 lib_key = 0;
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    i32 i = r2_find_locked(key);
    if (i >= 0) {
        str_set(path, g_r2.shares[i].path);
        str_set(title, g_r2.shares[i].title);
        str_set(game, g_r2.shares[i].game);
        lib_key = g_r2.shares[i].lib_key;
    }
    ReleaseSRWLockExclusive(&g_r2.lock);
    wchar_t wpath[MAX_PATH];
    utf8_to_wide(path, wpath, MAX_PATH);
    u32 id = r2_share_file(wpath, title, game, 0);
    for (i32 s = 0; s < COUNT(g_r2.jobs); s++)
        if (id && g_r2.jobs[s].id == id) {
            str_set(g_r2.work[s].reuse, key);
            g_r2.work[s].lib_key = lib_key;
        }
    return id;
}

static R2Job r2_job(u32 id)
{
    R2Job out = {};
    u32 save = 0;
    AcquireSRWLockShared(&g_r2.lock);
    for (i32 i = 0; i < COUNT(g_r2.jobs); i++)
        if (id && g_r2.jobs[i].id == id) {
            out = g_r2.jobs[i];
            save = g_r2.work[i].save;
        }
    ReleaseSRWLockShared(&g_r2.lock);
    if (out.state == R2_PREPARING && save) {
        const SaveJobView *v = save_view(save);
        if (v) out.progress = v->progress;
    }
    return out;
}

static bool r2_holds_save(u32 id)
{
    for (i32 i = 0; i < COUNT(g_r2.work); i++)
        if (id && g_r2.work[i].save == id) return true;
    return false;
}

static void r2_on_save_event(const Event &e)
{
    u32 id = (u32)(e.b & 0xffffffff);
    i32 slot = -1;
    for (i32 i = 0; i < COUNT(g_r2.work); i++)
        if (id && g_r2.work[i].save == id) slot = i;
    const SaveJobView *v = save_view(id);
    if (slot < 0 || !v || v->state < SAVE_DONE) return;
    R2Work &w = g_r2.work[slot];
    w.save = 0;
    if (v->state == SAVE_FAILED) {
        char why[256];
        snprintf(why, sizeof why, "Couldn't make the copy: %s", v->error);
        r2_fail_now(slot, why);
        return;
    }
    if (v->state == SAVE_CANCELLED) {
        r2_fail_now(slot, "Cancelled");
        return;
    }
    R2Task t = {};
    t.kind = r2_conf(&t.conf) ? R2_T_UPLOAD : R2_T_FREE;
    t.slot = slot;
    utf8_to_wide(v->path, t.file.s, MAX_PATH);
    str_set(t.replace, w.reuse);
    AcquireSRWLockShared(&g_r2.lock);
    i32 old = w.reuse[0] ? r2_find_locked(w.reuse) : -1;
    if (t.kind == R2_T_UPLOAD && old >= 0 && g_r2.shares[old].host == HOST_R2) str_set(t.key, w.reuse);
    ReleaseSRWLockShared(&g_r2.lock);
    str_set(t.hosts, g_app.config.link_hosts);
    t.hours = g_app.config.litterbox_hours;
    if (!r2_queue(t)) {
        DeleteFileW(t.file.s);
        r2_fail_now(slot, "Too many links are waiting");
    }
}

static void r2_on_event(const Event &e)
{
    if (e.a != R2_EV_PROBED) return; // the rest only wake the UI
    i32 slot = (i32)e.b;
    if (slot < 0 || slot >= COUNT(g_r2.work)) return;
    if (!g_r2.work[slot].has_info) r2_fail_now(slot, "Couldn't read the file");
    else r2_start_copy(slot);
}

// Uploads a small file, fetches it from the public address, deletes it. Reads the settings as they are now.
static void r2_test()
{
    R2Task t = {};
    t.kind = R2_T_TEST;
    bool ready = r2_conf(&t.conf);
    AcquireSRWLockExclusive(&g_r2.lock);
    g_r2.test.state = ready ? R2_TEST_BUSY : R2_TEST_FAILED;
    str_set(g_r2.test.text, ready ? "Testing..." : "Fill in every field and the secret key first");
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (ready && !r2_queue(t)) r2_test_end(false, "Busy: try again in a moment");
}

static R2View r2_test_view()
{
    AcquireSRWLockShared(&g_r2.lock);
    R2View v = g_r2.test;
    ReleaseSRWLockShared(&g_r2.lock);
    return v;
}

static bool r2_link_for(u64 lib_key, char *url, i32 cap)
{
    url[0] = 0;
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    i64 now = now_us();
    for (i32 i = g_r2.count - 1; i >= 0 && lib_key; i--)
        if (g_r2.shares[i].lib_key == lib_key && r2_share_live(g_r2.shares[i], now)) {
            snprintf(url, cap, "%s", g_r2.shares[i].url);
            break;
        }
    ReleaseSRWLockExclusive(&g_r2.lock);
    return url[0] != 0;
}

// The shares, newest first. Returns how many were written.
static i32 r2_history(R2Share *out, i32 max)
{
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    i32 n = 0;
    for (i32 i = g_r2.count - 1; i >= 0 && n < max; i--) out[n++] = g_r2.shares[i];
    ReleaseSRWLockExclusive(&g_r2.lock);
    return n;
}

// Changes whenever the history does.
static u32 r2_history_gen()
{
    AcquireSRWLockShared(&g_r2.lock);
    u32 g = g_r2.gen;
    ReleaseSRWLockShared(&g_r2.lock);
    return g;
}

static void r2_pin(const char *key, bool on)
{
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    i32 i = r2_find_locked(key);
    if (i >= 0 && g_r2.shares[i].pinned != on) {
        g_r2.shares[i].pinned = on;
        r2_save_locked();
    }
    ReleaseSRWLockExclusive(&g_r2.lock);
}

// Drops a share from the history, deleting it from the bucket first when it's there. A free host's copy stays
// there (Catbox keeps it, Litterbox drops it when it expires).
static void r2_delete(const char *key)
{
    R2Task t = {};
    t.kind = R2_T_DELETE;
    str_set(t.key, key);
    r2_conf(&t.conf);
    if (!r2_queue(t)) log_line("r2: too many tasks waiting; %s not deleted", key);
}

static R2Usage r2_usage()
{
    R2Usage u = {};
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    for (i32 i = 0; i < g_r2.count; i++)
        if (r2_in_bucket(g_r2.shares[i])) u.used_bytes += g_r2.shares[i].bytes;
    ReleaseSRWLockExclusive(&g_r2.lock);
    u.limit_bytes = r2_limit_for_budget(g_app.config.r2_budget);
    u.cost_per_gb_month = R2_PRICE_GB;
    u.free_gb = R2_FREE_GB;
    return u;
}
