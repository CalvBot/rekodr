// Links: a clip shared as a plain https link to an MP4 that Discord plays in place. Where it goes:
//   free hosts  with nothing set up, the copy goes to a free public host, the first in the player's order
//               (link_hosts) that takes its size: Catbox (up to 200 MB, kept, can't be deleted) or Litterbox (up to
//               1 GB, gone after litterbox_hours). Anyone with the link can watch. A host that fails passes the clip
//               to the next. link_hosts can also name the player's own uploaders (ShareX .sxcu files).
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
// The account, the token's id (the S3 access key), the bucket and its public address are in settings.txt; the
// tokens only in Windows' Credential Manager. Setup takes a sign-in with Cloudflare or a pasted API token.
// History is <config_dir>\shares.tsv. Network work runs on one worker thread; the main thread owns the jobs'
// steps and is told by EV_R2. This file never touches the clipboard: the UI copies the link.

#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

static void open_url(const char *url); // setup.cpp
static void r2_resume(const char *key);
static void r2_auto_pause();
struct R2Usage;
static R2Usage r2_usage();

enum R2State : u8 { R2_QUEUED, R2_PREPARING, R2_UPLOADING, R2_DONE, R2_FAILED };
// R2_SETUP_NO_TOKEN_SCOPE: signed in, but without API Tokens Write; the text says to turn it on and sign in again.
enum R2SetupState : u8 {
    R2_SETUP_IDLE,
    R2_SETUP_WAITING,
    R2_SETUP_BUSY,
    R2_SETUP_OK,
    R2_SETUP_FAILED,
    R2_SETUP_NO_TOKEN_SCOPE
};

struct R2Job {
    u32     id; // 0 = no such job
    R2State state;
    f32     progress; // 0..1 within the current step
    char    url[512]; // once done
    char    error[256];
};

// The setup's state: waiting for the browser's sign-in, busy with a step, or how it ended
struct R2Status {
    R2SetupState state;
    char         text[256]; // ready to show
    char         who[128];  // the Cloudflare account's name, once known
};

enum { R2_MAX_SHARES = 1024, R2_KEY_MAX = 48 };

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
    char key[R2_KEY_MAX]; // R2: object name in the bucket, "clipback/<16 random letters and digits>/clip.mp4"
                          // (with a preview page, the page is "clipback/<id>" and its picture
                          // "clipback/<id>/thumb.jpg");
                          // a free host: "<host>/<the file's name there>"; a custom uploader:
                          // "<its name in link_hosts>/<16 random>"
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
    u8   paused;      // R2: 0 plays, 1 paused by hand, 2 too many reads in a day (r2_read_rate), 3 the account
                      // near its month's reads (r2_read_cap)
    char hidden[R2_KEY_MAX]; // while paused: where the file is in the bucket instead, a new random name
    i32  pause_day;   // yyyymmdd (local): the day it was paused for its reads, or resumed by hand
    i32  page_w, page_h; // R2 with a preview page (share_link_page): the video's size; 0 = the link is the .mp4
    // Not kept in the history:
    bool moving;      // a pause or resume is running
    i64  plays_today, plays_month, plays_24h; // GET requests for the file (local day, this month, the last 24
                                              // hours), -1 until fetched
};

struct R2Usage {
    i64 used_bytes, limit_bytes; // what the links take up (the history), and what the budget allows
    f64 cost_per_gb_month, free_gb;
    i64 bucket_bytes;            // Cloudflare's count of what the bucket holds, -1 until fetched
    i64 class_a, class_b;        // requests to the bucket this month (UTC), -1 until fetched
    i64 usage_us;                // when those were fetched, 0 = never
    f64 month_cost;              // dollars this month at this rate, past the free allowances
    i64 account_reads;           // Class B requests to every bucket of the account this month, -1 until fetched
    bool capped;                 // those are near r2_read_cap: R2 links are paused until the month ends
};

// The addresses the bucket answers at, for Options' Public address
struct R2Domains {
    i8   state;           // 0 not asked, 1 asking, 2 listed, -1 Cloudflare wouldn't list them (`why`)
    char r2dev[128];      // the bucket's r2.dev domain, "" until known
    bool r2dev_on;
    char list[8][128];    // custom domains connected to the bucket, enabled and active
    i32  count;
    char why[160];
    i64  asked_us;
};

// What Cloudflare's analytics say about the bucket, and the account's reads
struct R2Fetched {
    i64 bytes, class_a, class_b, account_reads;
};

// Cloudflare's R2 prices: storage per GB-month, and per million Class A (writes, lists) and Class B (reads)
// requests, each with a free amount a month
static const f64 R2_FREE_GB = 10, R2_PRICE_GB = 0.015;
static const f64 R2_FREE_A = 1e6, R2_PRICE_A = 4.50, R2_FREE_B = 1e7, R2_PRICE_B = 0.36;
static const i64 R2_USAGE_EVERY_US = 3600ll * 1000000; // how often the analytics are asked
static const i64 R2_GB = 1000000000;
static const i64 R2_PART = 64ll << 20;       // multipart part size
static const i64 R2_SINGLE_MAX = 100ll << 20; // larger files go up in parts

enum R2EventKind : u8 { R2_EV_PROBED, R2_EV_JOB, R2_EV_STATUS, R2_EV_SETUP, R2_EV_HISTORY, R2_EV_USAGE, R2_EV_RATE };

// What a network task needs of the settings, copied when the task is queued.
struct R2Conf {
    char account[40], access[72], secret[96], bucket[64], public_url[256];
    i64  limit;
};

enum R2TaskKind : u8 { R2_T_PROBE, R2_T_UPLOAD, R2_T_FREE, R2_T_DELETE, R2_T_SETUP, R2_T_SIGNIN, R2_T_USAGE,
                        R2_T_SIGNOUT, R2_T_PAUSE, R2_T_RESUME, R2_T_DOMAINS, R2_T_CLEANUP };

struct R2Task {
    R2TaskKind kind;
    i32        slot;    // the job's slot (probe, upload, free)
    char       key[R2_KEY_MAX]; // delete: the share; upload: the name to reuse ("" = a new one)
    char       replace[R2_KEY_MAX]; // upload, free: the history entry this share takes the place of
    char       hosts[256]; // free: link_hosts
    i32        hours;     // free: litterbox_hours
    Path       file;    // probe: the file; upload: the copy to send (deleted after)
    Path       ffprobe;
    R2Conf     conf;
    bool       oauth;   // setup, usage: signed in with Cloudflare rather than a pasted token
    char       token[1024]; // setup, sign-in: the API token (pasted, or the bucket's own); usage: the pasted token;
                            // sign-out: the sign-in's refresh token
    char       code[1024];  // sign-in: the code the browser brought back
    char       verifier[64]; // sign-in: PKCE's verifier
    u8         pause;   // pause: 1 by hand, 2 past the day's plays; resume: 1 by hand
    bool       page;    // upload: with a preview page (share_link_page)
    Path       ffmpeg;  // upload with a page: for its picture
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
    char       hosts[64]; // where the link goes by Share's rule: "r2", "catbox", "litterbox" or an uploader's name
    i64        aim, limit; // the copy's size, as save_link takes them
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
    R2Status status;
    R2Conf   found;      // what the setup found (account, token id as access key, bucket, public address)
    R2Fetched fetched;
    i64      usage_us;   // when `fetched` came, 0 = never
    i64      usage_asked_us;
    R2Domains domains;
    i8       has_token; // 0 unknown, 1 yes, -1 no
    i8       signed_in; // the same for the sign-in's refresh token
    char     access[2048]; // worker: the sign-in's access token, until access_until_us
    i64      access_until_us;
    f64      budget;       // main thread: the monthly budget in force (r2_apply_budget), once budget_known
    bool     budget_known;
    i64      folder_bytes; // the bucket's clipback/ folder when last listed, 0 before
    struct {
        HANDLE        thread; // listens on CF_OAUTH_PORT for the browser
        volatile LONG cancel;
        char          state[48];
        R2Task        task; // the sign-in task, the code still to come
    } signin;
    R2Task  tasks[32];
    i32     task_count;
    HANDLE  thread, wake;
    volatile LONG stop;
} g_r2;

// ---------------------------------------------------------------------------------------------------------------
// Prices and limits

// What may be stored for a monthly budget, in bytes: all that the budget buys. Before each upload the bucket is
// listed for its real size and the oldest links are cleared to fit (r2_run_upload), so it never holds more.
static i64 r2_limit_for_budget(f64 dollars)
{
    if (dollars < 0) dollars = 0;
    return (i64)((R2_FREE_GB + dollars / R2_PRICE_GB) * (f64)R2_GB);
}

// Room kept for a preview page and its picture while a link's room is made; the entry counts what they really take.
static const i64 R2_PAGE_ROOM = 1 << 20;

static f64 r2_cost_for(i64 bytes)
{
    f64 gb = (f64)bytes / (f64)R2_GB - R2_FREE_GB;
    return gb > 0 ? gb * R2_PRICE_GB : 0;
}

// Why the bucket can't take a link now: its keys can't be read, or its account is near the month's reads
// (r2_read_cap). 0 when it can. Such a link fails with the reason; it never goes to a free host instead.
static const char *r2_refuses(bool keys, bool capped)
{
    if (!keys) return "Your Cloudflare keys can't be read: set up again in Options";
    if (capped) return "Links wait until the 1st: your Cloudflare account has nearly used the month's free reads";
    return 0;
}

// Adds one upload's speed (bytes a second) to a host's average of about its last 10.
static void r2_rate_add(f64 *avg, i32 *n, f64 rate)
{
    if (*n < 10) ++*n;
    *avg += (rate - *avg) / *n;
}

// The Catbox row's tooltip: Catbox's upload speed beside the bucket's ("" for no bucket), from the averages.
static void r2_rates_tip(const f64 *rate, const i32 *n, bool bucket, char *out, i32 cap)
{
    char a[32], b[32];
    if (n[HOST_CATBOX]) snprintf(a, sizeof a, "%.1f MB/s", rate[HOST_CATBOX] / 1e6);
    else str_set(a, "not measured yet");
    if (n[HOST_R2]) snprintf(b, sizeof b, "%.1f MB/s", rate[HOST_R2] / 1e6);
    else str_set(b, "not measured yet");
    if (bucket) snprintf(out, cap, "Upload speed from your last uploads: Catbox %s, your bucket %s", a, b);
    else snprintf(out, cap, "Upload speed from your last uploads: Catbox %s", a);
}

// In the bucket now: an R2 share that cleanup hasn't removed.
static bool r2_in_bucket(const R2Share &s) { return s.host == HOST_R2 && !s.removed; }

// The link still plays: not removed and not past a free host's expiry.
static bool r2_share_live(const R2Share &s, i64 now)
{
    return !s.removed && !s.paused && (!s.expires_us || now < s.expires_us);
}

// Where the share's file is in the bucket: its own name, or the hidden one while paused.
static const char *r2_object_key(const R2Share &s) { return s.paused ? s.hidden : s.key; }

// Today as yyyymmdd, local time.
static i32 r2_today()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    return t.wYear * 10000 + t.wMonth * 100 + t.wDay;
}

// Whether the account's reads this month are near `cap` (0 = no cap). R2's free reads are counted per account and
// per UTC month. Links pause at 90% of the cap: Cloudflare's analytics trail by some minutes and are read once an
// hour, and a busy link keeps being read in between.
static bool r2_capped(i64 reads, i64 cap)
{
    return cap > 0 && reads >= 0 && reads >= cap - cap / 10;
}

// What the automatic pause does to a share: 0 nothing, -1 resume, 2 pause for its reads in the last 24 hours past
// `rate` (0 = no limit), 3 pause while the account is `capped`. A link paused for its rate comes back the next day,
// one paused for the cap when the month's reads are under it again (the 1st). One resumed by hand keeps playing for
// the rest of that day; a link paused by hand stays paused.
static i32 r2_auto_action(const R2Share &s, i32 rate, bool capped, i32 today)
{
    if (s.host != HOST_R2 || s.removed || s.moving || s.paused == 1) return 0;
    if (s.paused == 3) return capped ? 0 : -1;
    if (s.paused == 2) return s.pause_day != today && !capped ? -1 : 0;
    if (s.pause_day == today) return 0;
    if (capped) return 3;
    return rate > 0 && s.plays_24h >= rate ? 2 : 0;
}

// A count of reads for a short status: "950", "12 K", "1.2 M", "10 M".
static void r2_fmt_reads(i64 n, char *out, i32 cap)
{
    if (n < 1000) snprintf(out, cap, "%lld", n);
    else if (n < 1000000) snprintf(out, cap, "%lld K", n / 1000);
    else if (n % 1000000 < 50000 || n >= 100000000) snprintf(out, cap, "%lld M", (n + 500000) / 1000000);
    else snprintf(out, cap, "%.1f M", (f64)n / 1e6);
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

// The bucket's links as the Space bar shows them, oldest first, into `out`, with goes[i] = 1 for those
// r2_plan_cleanup clears to fit `limit` with `incoming` more bytes and what the folder holds that the history
// doesn't know of (`untracked`). None go when the pinned links alone are over. Returns how many.
static i32 r2_evict_plan(const R2Share *s, i32 n, i64 limit, i64 incoming, i64 untracked, u8 *goes, R2Share *out,
                         i32 max)
{
    static i32 plan[R2_MAX_SHARES];
    i32 gone = r2_plan_cleanup(s, n, limit, incoming + untracked, -1, plan, COUNT(plan));
    i32 count = 0;
    for (i32 i = 0; i < n && count < max; i++) { // the history is oldest first
        if (!r2_in_bucket(s[i])) continue;
        out[count] = s[i];
        goes[count] = 0;
        for (i32 k = 0; k < gone; k++) goes[count] |= plan[k] == i;
        count++;
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
// Preview pages: a bucket link can be a small page that shows the clip, so chat apps show a card with its picture
// and play it. The page is "clipback/<id>" (text/html) beside the clip's folder, its picture
// "<the file's folder>/thumb.jpg": a frame a third of the way in, at most 1280 wide.

// The page's name for a share's key: "clipback/<id>/clip.mp4" -> "clipback/<id>".
static void r2_page_key(const char *key, char out[R2_KEY_MAX])
{
    const char *slash = strrchr(key, '/');
    i32 n = slash ? (i32)(slash - key) : (i32)strlen(key);
    snprintf(out, R2_KEY_MAX, "%.*s", n, key);
}

// The picture beside a file in the bucket: "clipback/<id>/clip.mp4" -> "clipback/<id>/thumb.jpg".
static void r2_thumb_key(const char *key, char out[R2_KEY_MAX])
{
    char dir[R2_KEY_MAX];
    r2_page_key(key, dir);
    snprintf(out, R2_KEY_MAX, "%s/thumb.jpg", dir);
}

// The picture's size for a video of w x h: at most 1280 wide, both even.
static void r2_thumb_size(i32 w, i32 h, i32 *tw, i32 *th)
{
    *tw = *th = 0;
    if (w <= 0 || h <= 0) return;
    i32 x = w < 1280 ? w : 1280;
    x &= ~1;
    i32 y = (i32)((i64)h * x / w + 1) & ~1;
    if (x < 2 || y < 2) return;
    *tw = x;
    *th = y;
}

// Appends `s` to out, made safe for an HTML attribute or text and ASCII only: & < > " ' as entities, anything past
// ASCII (UTF-8) as a numeric entity, control characters left out.
static void r2_html_text(const char *s, char *out, i32 cap, i32 *len)
{
    const u8 *p = (const u8 *)s;
    while (*p && *len < cap - 12) {
        u32 c = *p++;
        if (c >= 0x80) {
            i32 more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : -1;
            if (more < 0) continue;
            c &= 0x3f >> more;
            bool ok = true;
            for (i32 k = 0; k < more; k++) {
                if ((*p & 0xc0) != 0x80) { ok = false; break; }
                c = c << 6 | (*p++ & 0x3f);
            }
            if (ok && c >= 0xa0 && c <= 0x10ffff && (c < 0xd800 || c > 0xdfff))
                *len += snprintf(out + *len, cap - *len, "&#%u;", c);
            continue;
        }
        const char *e = c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '"' ? "&quot;"
                      : c == '\'' ? "&#39;" : 0;
        if (e) *len += snprintf(out + *len, cap - *len, "%s", e);
        else if (c >= 0x20 && c != 0x7f) out[(*len)++] = (char)c;
    }
    out[*len] = 0;
}

// A run of the page: text with %T (the title), %P (the page's address), %V (the video's), %I (the picture's),
// %W %H (the video's size) and %w %h (the picture's).
struct R2PageParts {
    const char *title, *page, *video, *image;
    i32 w, h, tw, th;
};
static void r2_page_put(const char *fmt, const R2PageParts &p, char *out, i32 cap, i32 *len)
{
    for (const char *f = fmt; *f && *len < cap - 1; f++) {
        if (*f != '%' || !f[1]) {
            out[(*len)++] = *f;
            continue;
        }
        char c = *++f;
        const char *s = c == 'T' ? p.title : c == 'P' ? p.page : c == 'V' ? p.video : c == 'I' ? p.image : 0;
        if (s) r2_html_text(s, out, cap, len);
        else {
            i32 n = c == 'W' ? p.w : c == 'H' ? p.h : c == 'w' ? p.tw : p.th;
            *len += snprintf(out + *len, cap - *len, "%d", n);
        }
    }
    out[*len < cap ? *len : cap - 1] = 0;
}

// The preview page for a link: the clip full width on a dark page, with the tags chat apps read for a card that
// plays it, and the plain APP_NAME word under it. Paused, the page names the clip and doesn't play it. All ASCII.
// Returns the length, 0 if it didn't fit.
static i32 r2_page_html(const char *title, const char *page_url, const char *video_url, const char *image_url, i32 w,
                        i32 h, i32 tw, i32 th, bool paused, char *out, i32 cap)
{
    if (cap < 2) return 0;
    R2PageParts p = {title && title[0] ? title : "Clip", page_url, video_url, image_url, w, h, tw, th};
    i32 len = 0;
    out[0] = 0;
    r2_page_put("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
                "<meta name=\"robots\" content=\"noindex\">\n<title>%T</title>\n"
                "<meta property=\"og:type\" content=\"video.other\">\n<meta property=\"og:title\" content=\"%T\">\n"
                "<meta property=\"og:site_name\" content=\"" APP_NAME "\">\n<meta property=\"og:url\" content=\"%P\">\n",
                p, out, cap, &len);
    if (!paused)
        r2_page_put("<meta property=\"og:image\" content=\"%I\">\n<meta property=\"og:image:width\" content=\"%w\">\n"
                    "<meta property=\"og:image:height\" content=\"%h\">\n<meta property=\"og:video\" content=\"%V\">\n"
                    "<meta property=\"og:video:secure_url\" content=\"%V\">\n"
                    "<meta property=\"og:video:type\" content=\"video/mp4\">\n"
                    "<meta property=\"og:video:width\" content=\"%W\">\n<meta property=\"og:video:height\" content=\"%H\">\n"
                    "<meta name=\"twitter:card\" content=\"player\">\n<meta name=\"twitter:title\" content=\"%T\">\n"
                    "<meta name=\"twitter:player\" content=\"%P\">\n<meta name=\"twitter:player:width\" content=\"%W\">\n"
                    "<meta name=\"twitter:player:height\" content=\"%H\">\n<meta name=\"twitter:image\" content=\"%I\">\n",
                    p, out, cap, &len);
    r2_page_put("<style>html,body{margin:0;min-height:100%;background:#0c0c0e;color:#7c7c86;"
                "font:13px/1.5 system-ui,sans-serif}video{display:block;width:100%;max-height:calc(100vh - 44px);"
                "background:#000}p{margin:0;padding:12px 16px;text-align:center;letter-spacing:.05em}</style>\n"
                "</head><body>\n",
                p, out, cap, &len);
    if (!paused)
        r2_page_put("<video src=\"%V\" poster=\"%I\" width=\"%W\" height=\"%H\" controls playsinline "
                    "preload=\"metadata\"></video>\n",
                    p, out, cap, &len);
    else r2_page_put("<p>This clip isn't playing right now.</p>\n", p, out, cap, &len);
    r2_page_put("<p>" APP_NAME "</p>\n</body></html>\n", p, out, cap, &len);
    return len < cap - 1 ? len : 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The API token and the sign-in's refresh token, in Windows' Credential Manager. A test copy (CLIPBACK_HOME) keeps
// its own. The API token is the one pasted, or with a sign-in the one clipback made for the bucket. The bucket's S3
// keys come from it: the access key is the token's id, the secret the SHA-256 of the token (r2_token_secret).

static void r2_cred_target(wchar_t *out, i32 cap, bool signin)
{
    u32 h = test_home_hash();
    const wchar_t *what = signin ? L"r2-signin" : L"r2";
    if (h) swprintf(out, cap, L"clipback-%08x/%ls", h, what);
    else swprintf(out, cap, L"clipback/%ls", what);
}

static bool r2_token_get(char *out, i32 cap, bool signin = false)
{
    out[0] = 0;
    wchar_t target[64];
    r2_cred_target(target, COUNT(target), signin);
    CREDENTIALW *cred = 0;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &cred)) return false;
    i32 n = (i32)cred->CredentialBlobSize;
    if (n >= cap) n = cap - 1;
    memcpy(out, cred->CredentialBlob, n);
    out[n] = 0;
    CredFree(cred);
    return out[0] != 0;
}

// Stores the token; "" removes it.
static void r2_token_store(const char *s, bool signin = false)
{
    wchar_t target[64];
    r2_cred_target(target, COUNT(target), signin);
    i8 &cache = signin ? g_r2.signed_in : g_r2.has_token;
    if (!s || !s[0]) {
        CredDeleteW(target, CRED_TYPE_GENERIC, 0);
        cache = -1;
        return;
    }
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = target;
    cred.CredentialBlobSize = (DWORD)strlen(s);
    cred.CredentialBlob = (LPBYTE)s;
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = (wchar_t *)L"clipback";
    if (CredWriteW(&cred, 0)) cache = 1;
    else log_line("r2: couldn't store the %s (%d)", signin ? "sign-in" : "API token", (int)GetLastError());
}

static bool r2_has_token()
{
    if (!g_r2.has_token) {
        char s[256];
        g_r2.has_token = r2_token_get(s, sizeof s) ? 1 : -1;
        SecureZeroMemory(s, sizeof s);
    }
    return g_r2.has_token > 0;
}

// Signed in with Cloudflare (a refresh token is kept), rather than with a pasted token.
static bool r2_signed_in()
{
    if (!g_r2.signed_in) {
        char s[1024];
        g_r2.signed_in = r2_token_get(s, sizeof s, true) ? 1 : -1;
        SecureZeroMemory(s, sizeof s);
    }
    return g_r2.signed_in > 0;
}

static bool r2_ready()
{
    const Config &c = g_app.config;
    return c.r2_account[0] && c.r2_access_key[0] && c.r2_bucket[0] && c.r2_public_url[0] && r2_has_token();
}

// The bucket has been set up: links go there, never to a free host.
static bool r2_set_up()
{
    const Config &c = g_app.config;
    return c.r2_account[0] && c.r2_access_key[0];
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

// The bucket's S3 secret key for an API token: the token's SHA-256 in lower-case hex
static void r2_token_secret(const char *token, char out[65])
{
    u8 h[32];
    out[0] = 0;
    if (r2_sha256(token, strlen(token), h)) r2_hex(h, 32, out);
    SecureZeroMemory(h, sizeof h);
}

// The monthly budget in force: what was saved, until r2_apply_budget sets a new one. Options' draft in the
// settings never changes it.
static f64 r2_budget_now()
{
    if (!g_r2.budget_known) {
        g_r2.budget = g_app.config.r2_budget;
        g_r2.budget_known = true;
    }
    return g_r2.budget;
}

static bool r2_conf(R2Conf *out)
{
    const Config &c = g_app.config;
    *out = {};
    str_set(out->account, c.r2_account);
    str_set(out->access, c.r2_access_key);
    str_set(out->bucket, c.r2_bucket);
    str_set(out->public_url, c.r2_public_url);
    out->limit = r2_limit_for_budget(r2_budget_now());
    char token[256];
    if (r2_token_get(token, sizeof token)) r2_token_secret(token, out->secret);
    SecureZeroMemory(token, sizeof token);
    return out->account[0] && out->access[0] && out->secret[0] && out->bucket[0] && out->public_url[0];
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
                   R2Progress *prog = 0, const char *copy_from = 0)
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
    // CopyObject names its source in x-amz-copy-source, signed with the rest (in name order)
    char source[160] = "";
    if (copy_from) snprintf(source, sizeof source, "/%s/%s", c.bucket, copy_from);
    SigHeader h[] = {{"host", host},
                     {"x-amz-content-sha256", "UNSIGNED-PAYLOAD"},
                     {copy_from ? "x-amz-copy-source" : "x-amz-date", copy_from ? source : date},
                     {"x-amz-date", date}};
    sigv4_auth(method, uri, query, h, copy_from ? 4 : 3, "UNSIGNED-PAYLOAD", c.access, c.secret, date, "auto", "s3",
               auth, sizeof auth);
    snprintf(hdrs, sizeof hdrs,
             "x-amz-content-sha256: UNSIGNED-PAYLOAD\r\nx-amz-date: %s\r\nAuthorization: %s%s%s%s%s", date, auth,
             content_type ? "\r\nContent-Type: " : "", content_type ? content_type : "",
             copy_from ? "\r\nx-amz-copy-source: " : "", source);

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
    if (total >= 0) {
        AcquireSRWLockExclusive(&g_r2.lock);
        g_r2.folder_bytes = total;
        ReleaseSRWLockExclusive(&g_r2.lock);
    }
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
//   key  version  pinned  removed  bytes  uploaded_us  lib_key  host  expires_us  paused  hidden  pause_day  url
//   title  game  path

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
        fprintf(f, "%s\t%d\t%d\t%d\t%lld\t%lld\t%llu\t%d\t%lld\t%d\t%s\t%d\t%d\t%d\t", s[i].key, s[i].version,
                s[i].pinned ? 1 : 0, s[i].removed ? 1 : 0, s[i].bytes, s[i].uploaded_us, s[i].lib_key, (int)s[i].host,
                s[i].expires_us, (int)s[i].paused, s[i].hidden, s[i].pause_day, s[i].page_w, s[i].page_h);
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
        char *f[18] = {};
        i32 count = 0;
        for (char *p = line; count < 18;) {
            f[count++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }
        if (count == 18 && strlen(f[0]) < R2_KEY_MAX && strlen(f[10]) < R2_KEY_MAX && atoi(f[7]) >= 0 &&
            atoi(f[7]) < HOST_COUNT) {
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
            s.paused = (u8)atoi(f[9]);
            str_set(s.hidden, f[10]);
            s.pause_day = atoi(f[11]);
            if (s.paused > 3 || (s.paused && !s.hidden[0])) s.paused = 0;
            s.page_w = atoi(f[12]);
            s.page_h = atoi(f[13]);
            if (s.page_w <= 0 || s.page_h <= 0) s.page_w = s.page_h = 0;
            str_set(s.url, f[14]);
            str_set(s.title, f[15]);
            str_set(s.game, f[16]);
            str_set(s.path, f[17]);
            s.plays_today = s.plays_month = s.plays_24h = -1;
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

// The share a name in the bucket belongs to: its file (where it is now), and with a page, the page and picture.
static i32 r2_find_object_locked(const char *name)
{
    for (i32 i = 0; i < g_r2.count; i++) {
        const R2Share &s = g_r2.shares[i];
        if (!strcmp(s.key, name) || (s.paused && !strcmp(s.hidden, name))) return i;
        if (!s.page_w) continue;
        char k[R2_KEY_MAX];
        r2_page_key(s.key, k);
        if (!strcmp(k, name)) return i;
        r2_thumb_key(r2_object_key(s), k);
        if (!strcmp(k, name)) return i;
    }
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
        if (!r2_share_live(g_r2.shares[i], now) && !r2_in_bucket(g_r2.shares[i])) { drop = i; break; }
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

static bool lib_probe(const Path &ffprobe, const Path &file, MediaInfo *m, char *tagged, i32 tagged_cap);

static bool r2_put_bytes(const R2Conf &c, const char *key, const char *data, i64 len, const char *type)
{
    char resp[2048];
    u32 status = r2_send(c, "PUT", key, "", R2Body{data, 0, 0, len}, type, resp, sizeof resp);
    if (!r2_ok(status)) log_line("r2: couldn't write %s (%u)", key, status);
    return r2_ok(status);
}

// Writes a share's preview page (under the lock's copy `s`): playing, or paused.
static bool r2_put_page(const R2Conf &c, const R2Share &s, bool paused)
{
    char page_key[R2_KEY_MAX], thumb_key[R2_KEY_MAX], page[512], video[512], image[512];
    r2_page_key(s.key, page_key);
    r2_thumb_key(s.key, thumb_key);
    r2_make_url(c.public_url, page_key, s.version, page, sizeof page);
    r2_make_url(c.public_url, s.key, s.version, video, sizeof video);
    r2_make_url(c.public_url, thumb_key, s.version, image, sizeof image);
    i32 tw, th;
    r2_thumb_size(s.page_w, s.page_h, &tw, &th);
    static thread_local char html[8192];
    i32 n = r2_page_html(s.title, page, video, image, s.page_w, s.page_h, tw, th, paused, html, sizeof html);
    return n > 0 && r2_put_bytes(c, page_key, html, n, "text/html; charset=utf-8");
}

// The preview page's picture from the copy: a frame a third of the way in. Fills the video's size; false (and
// nothing made) when ffmpeg can't.
static bool r2_make_thumb(const Path &file, const Path &ffmpeg, const Path &jpg, i32 *w, i32 *h)
{
    MediaInfo m = {};
    char tagged[256];
    i32 tw, th;
    if (!ffmpeg.s[0] || !lib_probe(ffprobe_for(ffmpeg), file, &m, tagged, sizeof tagged)) return false;
    r2_thumb_size(m.width, m.height, &tw, &th);
    if (!tw) return false;
    static thread_local wchar_t args[2048];
    args[0] = 0;
    char at[32], scale[64];
    snprintf(at, sizeof at, "%.3f", m.duration > 0 ? m.duration / 3 : 0);
    snprintf(scale, sizeof scale, "scale=%d:%d", tw, th);
    const char *head[] = {"-hide_banner", "-loglevel", "error", "-y", "-ss", at, "-i"};
    arg_list(args, COUNT(args), head, COUNT(head));
    cmd_arg(args, COUNT(args), file.s);
    const char *tail[] = {"-an", "-frames:v", "1", "-vf", scale, "-q:v", "3"};
    arg_list(args, COUNT(args), tail, COUNT(tail));
    cmd_arg(args, COUNT(args), jpg.s);
    if (proc_run(ffmpeg.s, args, 30000, 0, 0) != 0 || !file_exists(jpg.s)) {
        DeleteFileW(jpg.s);
        return false;
    }
    *w = m.width;
    *h = m.height;
    return true;
}

// Deletes what a share has in the bucket: the file at `at` (where it is now), and with a page the picture beside
// it and the page. Only the file's delete decides; the page and picture are tried once.
static bool r2_delete_share(const R2Conf &c, const char *key, const char *at, bool page)
{
    if (!r2_delete_object(c, at)) return false;
    if (page) {
        char k[R2_KEY_MAX];
        r2_thumb_key(at, k);
        r2_delete_object(c, k);
        r2_page_key(key, k);
        r2_delete_object(c, k);
    }
    return true;
}

// Clears the shares at `plan` (indexes, under the lock when it was made) from the bucket, keeping their entries so
// they can be shared again. Returns whether all of them went.
static bool r2_remove_planned(const R2Conf &c, const i32 *plan, i32 n)
{
    static char gone[R2_MAX_SHARES][R2_KEY_MAX];
    static char gone_at[R2_MAX_SHARES][R2_KEY_MAX]; // where each is in the bucket
    static bool gone_page[R2_MAX_SHARES];
    AcquireSRWLockShared(&g_r2.lock);
    for (i32 i = 0; i < n; i++) {
        str_set(gone[i], g_r2.shares[plan[i]].key);
        str_set(gone_at[i], r2_object_key(g_r2.shares[plan[i]]));
        gone_page[i] = g_r2.shares[plan[i]].page_w > 0;
    }
    ReleaseSRWLockShared(&g_r2.lock);
    bool all = true;
    for (i32 i = 0; i < n; i++) {
        if (!r2_delete_share(c, gone[i], gone_at[i], gone_page[i])) {
            all = false;
            continue;
        }
        AcquireSRWLockExclusive(&g_r2.lock);
        i32 k = r2_find_locked(gone[i]);
        if (k >= 0) {
            g_r2.shares[k].removed = true;
            g_r2.shares[k].paused = 0;
            g_r2.shares[k].hidden[0] = 0;
        }
        r2_save_locked();
        ReleaseSRWLockExclusive(&g_r2.lock);
        log_line("r2: removed %s to stay under the limit", gone[i]);
    }
    if (n > 0) r2_post(R2_EV_HISTORY, 0);
    return all;
}

// The upload step of a job, then the cleanup.
// Tells the main thread how fast an upload went, for the host's average (Config.upload_rate). Clips under 1 MB
// say more about the wait for an answer than about speed and aren't counted.
static void r2_rate_note(LinkHost host, i64 bytes, i64 us)
{
    if (bytes < 1000000 || us <= 0 || host > HOST_LITTERBOX) return;
    i64 rate = (i64)(bytes * 1e6 / us);
    r2_post(R2_EV_RATE, rate << 2 | host);
}

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

    // Room first, from the bucket's real size: the oldest unpinned links go until this one, its page and picture
    // fit. Uploads run one at a time on this thread, so two can't both count the same room.
    i64 folder = r2_folder_bytes(t.conf);
    i64 incoming = bytes + (t.page ? R2_PAGE_ROOM : 0);
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    i32 reuse = t.key[0] ? r2_find_locked(t.key) : -1;
    i32 clear = folder < 0 ? 0 : r2_plan_cleanup(g_r2.shares, g_r2.count, t.conf.limit,
                                                incoming + r2_untracked_locked(folder), reuse, plan, COUNT(plan));
    if (folder < 0) str_set(err, "Couldn't read how full the bucket is: try again");
    else if (clear < 0)
        str_set(err, incoming > t.conf.limit ? "The clip is bigger than your limit: raise it in Options"
                                             : "Pinned clips fill your limit: unpin some or raise it");
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (!err[0] && clear > 0 && !r2_remove_planned(t.conf, plan, clear))
        str_set(err, "Couldn't make room in the bucket: try again");
    if (reuse >= 0) str_set(key, t.key);
    else r2_new_key(key);

    i64 began = now_us();
    if (!err[0]) r2_put_file(t.conf, key, t.file, job, err, sizeof err);
    if (!err[0]) r2_rate_note(HOST_R2, bytes, now_us() - began);
    // The preview page's picture; without one the link is the file itself.
    i32 page_w = 0, page_h = 0;
    i64 extra = 0;
    if (!err[0] && t.page) {
        Path jpg = t.file;
        wcscat_s(jpg.s, MAX_PATH, L".jpg");
        char *pic = 0;
        i64 pic_len = 0;
        if (r2_make_thumb(t.file, t.ffmpeg, jpg, &page_w, &page_h)) pic = read_file(jpg.s, &pic_len);
        DeleteFileW(jpg.s);
        char thumb_key[R2_KEY_MAX];
        r2_thumb_key(key, thumb_key);
        if (!pic || !r2_put_bytes(t.conf, thumb_key, pic, pic_len, "image/jpeg")) page_w = page_h = 0;
        else extra = pic_len + 4096;
        free(pic);
        if (!page_w) log_line("r2: no preview page for %s: its picture couldn't be made", key);
    }
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
    s.paused = 0;
    s.hidden[0] = 0;
    s.host = HOST_R2;
    s.expires_us = 0;
    s.bytes = bytes + extra;
    s.uploaded_us = now_us();
    s.lib_key = w.lib_key;
    str_set(s.title, w.title);
    str_set(s.game, w.game);
    wide_to_utf8(w.src.s, s.path, sizeof s.path);
    s.page_w = page_w;
    s.page_h = page_h;
    R2Share page = s;
    if (page_w) {
        char page_key[R2_KEY_MAX];
        r2_page_key(key, page_key);
        r2_make_url(t.conf.public_url, page_key, s.version, s.url, sizeof s.url);
    } else r2_make_url(t.conf.public_url, key, s.version, s.url, sizeof s.url);
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (page_w && !r2_put_page(t.conf, page, false)) { // no page: the link is the file
        AcquireSRWLockExclusive(&g_r2.lock);
        at = r2_find_locked(key);
        if (at >= 0) {
            g_r2.shares[at].page_w = g_r2.shares[at].page_h = 0;
            r2_make_url(t.conf.public_url, key, g_r2.shares[at].version, g_r2.shares[at].url,
                        sizeof g_r2.shares[at].url);
        }
        ReleaseSRWLockExclusive(&g_r2.lock);
    }
    folder = r2_folder_bytes(t.conf); // the real total, with this clip in it
    AcquireSRWLockExclusive(&g_r2.lock);
    at = r2_find_locked(key);
    if (at < 0) { // gone while unlocked (deleted): nothing to hand out
        ReleaseSRWLockExclusive(&g_r2.lock);
        r2_job_end(job, false, "", "The link was deleted");
        return;
    }
    str_set(url, g_r2.shares[at].url);
    // The page or picture came out bigger than their room: the oldest others go now.
    i32 n = folder < 0 ? 0 : r2_plan_cleanup(g_r2.shares, g_r2.count, t.conf.limit, r2_untracked_locked(folder), at,
                                             plan, COUNT(plan));
    r2_save_locked();
    ReleaseSRWLockExclusive(&g_r2.lock);
    r2_job_end(job, true, url, "");
    if (n > 0) r2_remove_planned(t.conf, plan, n);
}

// After a new budget: the oldest unpinned links go until the bucket, at its real size, fits the limit.
static void r2_run_cleanup(const R2Task &t)
{
    static i32 plan[R2_MAX_SHARES];
    i64 folder = r2_folder_bytes(t.conf);
    if (folder < 0) {
        log_line("r2: couldn't read how full the bucket is to fit the new budget");
        return;
    }
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    i32 n = r2_plan_cleanup(g_r2.shares, g_r2.count, t.conf.limit, r2_untracked_locked(folder), -1, plan,
                            COUNT(plan));
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (n < 0) log_line("r2: pinned links alone are over the new budget: nothing removed");
    else if (n > 0) r2_remove_planned(t.conf, plan, n);
}

// ---------------------------------------------------------------------------------------------------------------
// Custom uploaders: ShareX's .sxcu files in <config_dir>\uploaders, named in link_hosts by file name without
// ".sxcu". What's taken of one: RequestMethod (POST, PUT or PATCH), RequestURL (https only), Parameters, Headers,
// Body (MultipartFormData with Arguments and FileFormName, or Binary) and URL, the link made from the answer:
// {response} / $response$ for all of it, {json:a.b[0].c} / $json:a.b[0].c$ for a value in it. {filename} in a
// value is "clip.mp4".

enum { UP_PAIRS = 12 };

struct Uploader {
    char name[64];
    char method[8];
    char url[1024];      // RequestURL with Parameters added
    char form_name[64];  // "" = Binary: the file is the whole body
    char result[512];    // URL
    char pairs[2][UP_PAIRS][2][256]; // [0] Headers, [1] Arguments: name, value
    i32  count[2];
};

// After a key in the object at j->p. False if there's none.
static bool j_key(Json *j, const char *k, size_t kl)
{
    if (!j_eat(j, '{')) return false;
    while (!j_eat(j, '}') && j->p < j->end) {
        char name[128];
        if (!j_string(j, name, sizeof name)) return false;
        j_eat(j, ':');
        if (strlen(name) == kl && !strncmp(name, k, kl)) { j_ws(j); return true; }
        j_skip(j);
        j_eat(j, ',');
    }
    return false;
}

// A string or number at a path with array indexes ("files[0].url", "[0].link"), as text.
static bool json_path(const char *text, i64 len, const char *path, char *out, i32 cap)
{
    Json j = {text, text + len};
    out[0] = 0;
    for (const char *p = path; *p;) {
        if (*p == '.') { p++; continue; }
        if (*p == '[') {
            i32 n = atoi(p + 1);
            const char *close = strchr(p, ']');
            if (!close || !j_eat(&j, '[')) return false;
            for (i32 i = 0; i < n; i++) {
                j_skip(&j);
                if (!j_eat(&j, ',')) return false;
            }
            j_ws(&j);
            p = close + 1;
            continue;
        }
        size_t kl = strcspn(p, ".[");
        if (!j_key(&j, p, kl)) return false;
        p += kl;
    }
    j_ws(&j);
    if (j.p >= j.end) return false;
    if (*j.p == '"') return j_string(&j, out, cap) && out[0];
    if (*j.p == '{' || *j.p == '[') return false;
    i32 n = 0;
    while (j.p < j.end && *j.p != ',' && *j.p != '}' && *j.p != ']' && (u8)*j.p > ' ' && n < cap - 1) out[n++] = *j.p++;
    out[n] = 0;
    return n > 0 && strcmp(out, "null");
}

// The link from an uploader's URL template and the host's answer. False when a {json:...} isn't in the answer.
static bool uploader_result(const char *tmpl, const char *resp, char *out, i32 cap)
{
    i32 n = 0;
    out[0] = 0;
    char *body = (char *)resp;
    while (*body && (u8)*body <= ' ') body++;
    i32 blen = (i32)strlen(body);
    while (blen > 0 && (u8)body[blen - 1] <= ' ') blen--;
    if (!tmpl[0]) return snprintf(out, cap, "%.*s", blen, body) < cap && blen > 0;
    for (const char *p = tmpl; *p && n < cap - 1;) {
        bool dollar = *p == '$';
        if ((*p == '{' || dollar) && (starts_with(p + 1, "json:") || starts_with(p + 1, "response"))) {
            const char *end = strchr(p + 1, dollar ? '$' : '}');
            if (!end) return false;
            char val[1024] = "";
            if (starts_with(p + 1, "response")) snprintf(val, sizeof val, "%.*s", blen, body);
            else {
                char path[256];
                snprintf(path, sizeof path, "%.*s", (int)(end - p - 6), p + 6);
                if (!json_path(resp, (i64)strlen(resp), path, val, sizeof val)) return false;
            }
            n += snprintf(out + n, cap - n, "%s", val);
            if (n >= cap) n = cap - 1;
            p = end + 1;
        } else out[n++] = *p++;
    }
    out[n] = 0;
    return n > 0;
}

// `{filename}` in a value becomes "clip.mp4".
static void uploader_fill(const char *in, char *out, i32 cap)
{
    i32 n = 0;
    for (const char *p = in; *p && n < cap - 1;) {
        if (starts_with(p, "{filename}")) {
            n += snprintf(out + n, cap - n, "clip.mp4");
            if (n >= cap) n = cap - 1;
            p += 10;
        } else out[n++] = *p++;
    }
    out[n] = 0;
}

// The pairs of a top-level object ("Headers"), string values only. Returns how many.
static i32 uploader_pairs(const char *text, i64 len, const char *key, char (*out)[2][256], i32 max)
{
    Json j = {text, text + len};
    if (!j_key(&j, key, strlen(key)) || !j_eat(&j, '{')) return 0;
    i32 n = 0;
    while (!j_eat(&j, '}') && j.p < j.end) {
        char k[256], v[256];
        if (!j_string(&j, k, sizeof k)) break;
        j_eat(&j, ':');
        j_ws(&j);
        if (j.p < j.end && *j.p == '"' && j_string(&j, v, sizeof v) && n < max) {
            uploader_fill(k, out[n][0], 256);
            uploader_fill(v, out[n][1], 256);
            n++;
        } else j_skip(&j);
        j_eat(&j, ',');
    }
    return n;
}

// Reads an .sxcu file's text. False, with why, when clipback can't use it.
static bool uploader_parse(const char *text, i64 len, Uploader *u, char *why, i32 cap)
{
    *u = {};
    why[0] = 0;
    char body[32] = "", reqtype[32] = "";
    j_get(text, len, "Name", u->name, sizeof u->name);
    if (!j_get(text, len, "RequestMethod", u->method, sizeof u->method) &&
        !j_get(text, len, "RequestType", u->method, sizeof u->method))
        str_set(u->method, "POST");
    j_get(text, len, "RequestURL", u->url, sizeof u->url);
    j_get(text, len, "Body", body, sizeof body);
    j_get(text, len, "FileFormName", u->form_name, sizeof u->form_name);
    j_get(text, len, "URL", u->result, sizeof u->result);
    j_get(text, len, "DestinationType", reqtype, sizeof reqtype);
    for (char *c = u->method; *c; c++) *c = (char)toupper((u8)*c);
    u->count[0] = uploader_pairs(text, len, "Headers", u->pairs[0], UP_PAIRS);
    u->count[1] = uploader_pairs(text, len, "Arguments", u->pairs[1], UP_PAIRS);
    char params[UP_PAIRS][2][256];
    i32 np = uploader_pairs(text, len, "Parameters", params, UP_PAIRS);
    for (i32 i = 0; i < np; i++) {
        char k[768], v[768];
        r2_uri_encode(params[i][0], k, sizeof k);
        r2_uri_encode(params[i][1], v, sizeof v);
        i32 at = (i32)strlen(u->url);
        snprintf(u->url + at, sizeof u->url - at, "%s%s=%s", strchr(u->url, '?') ? "&" : "?", k, v);
    }
    if (!body[0]) str_set(body, u->form_name[0] ? "MultipartFormData" : "Binary");
    if (!starts_with(u->url, "https://")) snprintf(why, cap, "It must send to an https:// address");
    else if (strcmp(u->method, "POST") && strcmp(u->method, "PUT") && strcmp(u->method, "PATCH"))
        snprintf(why, cap, "It sends with %s: only POST, PUT and PATCH upload", u->method);
    else if (!strcmp(body, "MultipartFormData") && !u->form_name[0])
        snprintf(why, cap, "It has no FileFormName for the file");
    else if (strcmp(body, "MultipartFormData") && strcmp(body, "Binary"))
        snprintf(why, cap, "It sends its body as %s: only MultipartFormData and Binary carry a clip", body);
    else if (strstr(u->result, "{regex:") || strstr(u->result, "$regex:") || strstr(u->result, "{xml:") ||
             strstr(u->result, "$xml:"))
        snprintf(why, cap, "Its URL reads the answer with regex or xml: only {response} and {json:...} work");
    if (!strcmp(body, "Binary")) u->form_name[0] = 0;
    if (!u->name[0]) str_set(u->name, "Custom uploader");
    return !why[0];
}

static Path uploader_dir() { return path_join(g_app.paths.config_dir, L"uploaders"); }

// Loads <config_dir>\uploaders\<stem>.sxcu. False, with why, when it's missing or unusable.
static bool uploader_load(const char *stem, Uploader *u, char *why, i32 cap)
{
    wchar_t w[128], name[160];
    utf8_to_wide(stem, w, COUNT(w));
    swprintf(name, COUNT(name), L"%s.sxcu", w);
    i64 len = 0;
    char *text = wcspbrk(w, L"\\/:") ? 0 : read_file(path_join(uploader_dir(), name).s, &len);
    if (!text) {
        snprintf(why, cap, "There's no uploader named %s", stem);
        return false;
    }
    bool ok = uploader_parse(text, len, u, why, cap);
    free(text);
    return ok;
}

// The uploaders in the folder, by file name without .sxcu, sorted. Returns how many.
static i32 uploader_list(char (*stems)[64], i32 max)
{
    Path pattern = path_join(uploader_dir(), L"*.sxcu");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.s, &fd);
    i32 n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char s[MAX_PATH];
        wide_to_utf8(fd.cFileName, s, sizeof s);
        char *dot = strrchr(s, '.');
        if (dot) *dot = 0;
        if (n < max && s[0] && strlen(s) < 64 && !strchr(s, ',')) str_set(stems[n++], s);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(stems, n, sizeof stems[0], [](const void *a, const void *b) { return _stricmp((const char *)a, (const char *)b); });
    return n;
}

// Copies an .sxcu file into the uploaders folder after checking it. Returns the name to put in link_hosts, "" with
// why when it can't be used.
static bool uploader_add(const wchar_t *file, char *stem, i32 stem_cap, char *why, i32 cap)
{
    stem[0] = 0;
    i64 len = 0;
    char *text = read_file(file, &len);
    if (!text) {
        snprintf(why, cap, "Couldn't read the file");
        return false;
    }
    Uploader u;
    bool ok = uploader_parse(text, len, &u, why, cap);
    free(text);
    if (!ok) return false;
    const wchar_t *base = wcsrchr(file, L'\\');
    base = base ? base + 1 : file;
    char name[MAX_PATH];
    wide_to_utf8(base, name, sizeof name);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    for (char *c = name; *c; c++)
        if (*c == ',' || *c == '/' || *c == '\\' || *c == ':') *c = '-';
    for (const FreeHost &f : FREE_HOSTS)
        if (!_stricmp(name, f.name)) strncat(name, "-custom", sizeof name - strlen(name) - 1);
    snprintf(stem, stem_cap, "%.60s", name);
    Path dir = uploader_dir();
    CreateDirectoryW(dir.s, 0);
    wchar_t w[128], target[160];
    utf8_to_wide(stem, w, COUNT(w));
    swprintf(target, COUNT(target), L"%s.sxcu", w);
    if (!CopyFileW(file, path_join(dir, target).s, FALSE)) {
        snprintf(why, cap, "Couldn't copy it to the uploaders folder");
        stem[0] = 0;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Uploads to free hosts and custom uploaders

// One upload request: a form with the file in `form_name` ("clip.mp4", video/mp4) after `fields`, or with no
// form_name the file as the whole body.
struct UpRequest {
    const char *method, *url;   // https://host[:port]/path?query
    const char *headers;        // "Name: value\r\n..." or ""
    const char *(*fields)[2];
    i32         nfields;
    const char *form_name;
};

// Sends the request with the file, the answer read into resp. Returns the status, 0 if the host couldn't be
// reached.
static u32 up_send(const UpRequest &q, HANDLE f, i64 size, R2Progress *prog, char *resp, i32 cap)
{
    resp[0] = 0;
    wchar_t wurl[1024], host[256], path[1024];
    utf8_to_wide(q.url, wurl, COUNT(wurl));
    URL_COMPONENTS uc = {sizeof uc};
    uc.lpszHostName = host;
    uc.dwHostNameLength = COUNT(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = COUNT(path);
    wchar_t extra[1024] = L"";
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = COUNT(extra);
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return 0;
    wcsncat(path, extra, COUNT(path) - wcslen(path) - 1);

    char boundary[48], pre[8192], post[64];
    i32 len = 0, post_len = 0;
    wchar_t hdrs[2600];
    char rnd[25];
    r2_random(rnd, 24);
    snprintf(boundary, sizeof boundary, "clipback%s", rnd);
    if (q.form_name && q.form_name[0]) {
        for (i32 i = 0; i < q.nfields; i++)
            len += snprintf(pre + len, sizeof pre - len,
                            "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", boundary,
                            q.fields[i][0], q.fields[i][1]);
        len += snprintf(pre + len, sizeof pre - len,
                        "--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"clip.mp4\"\r\n"
                        "Content-Type: video/mp4\r\n\r\n",
                        boundary, q.form_name);
        if (len >= (i32)sizeof pre) return 0;
        post_len = snprintf(post, sizeof post, "\r\n--%s--\r\n", boundary);
        swprintf(hdrs, COUNT(hdrs), L"%hsContent-Type: multipart/form-data; boundary=%hs", q.headers, boundary);
    } else swprintf(hdrs, COUNT(hdrs), L"%hsContent-Type: video/mp4", q.headers);
    wchar_t method[8];
    utf8_to_wide(q.method, method, COUNT(method));

    u32 status = 0;
    HINTERNET session = WinHttpOpen(L"clipback", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host, uc.nPort, 0) : 0;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, method, path, 0, 0, 0, WINHTTP_FLAG_SECURE) : 0;
    if (req) {
        WinHttpSetTimeouts(req, 15000, 15000, 120000, 300000); // the host answers once the whole file is in
        DWORD total = (DWORD)(len + size + post_len), wrote = 0;
        LARGE_INTEGER zero = {};
        bool sent = SetFilePointerEx(f, zero, 0, FILE_BEGIN) &&
                    WinHttpSendRequest(req, hdrs, (DWORD)-1L, 0, 0, total, 0) &&
                    (!len || (WinHttpWriteData(req, pre, len, &wrote) && wrote == (DWORD)len));
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
        sent = sent && (!post_len || (WinHttpWriteData(req, post, post_len, &wrote) && wrote == (DWORD)post_len));
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
        if (!status) DEV_LOG("r2: %ls: no answer (error %d)", host, (int)GetLastError());
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    return status;
}

// A link from what a host answered: a plain https address with nothing else on the line.
static bool up_plain_link(const char *resp, char *out, i32 cap)
{
    return uploader_result("", resp, out, cap) && starts_with(out, "https://") && !strpbrk(out, " \r\n<\"");
}

// The upload step of a job with no R2: each host in link_hosts' order that takes the size (a free host, or a
// custom uploader by name), until one gives a link.
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

    char err[256] = "", url[512] = "", used_name[64] = "";
    const FreeHost *used = 0;
    HANDLE f = CreateFileW(t.file.s, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    LARGE_INTEGER size = {};
    if (f == INVALID_HANDLE_VALUE) str_set(err, "Couldn't open the copy to upload");
    else GetFileSizeEx(f, &size);
    i32 tried = 0, named = 0;
    char hours[8];
    snprintf(hours, sizeof hours, "%dh", free_hours(t.hours));
    static thread_local char resp[16384];
    static thread_local Uploader u;
    for (const char *p = t.hosts; *p && !err[0] && !url[0] && !g_r2.stop;) {
        const char *end = strchr(p, ',');
        char name[64];
        snprintf(name, sizeof name, "%.*s", end ? (int)(end - p) : (int)strlen(p), p);
        p = end ? end + 1 : p + strlen(p);
        char *nm = trim(name);
        if (!nm[0]) continue;
        named++;
        const FreeHost *h = 0;
        for (const FreeHost &fh : FREE_HOSTS)
            if (!_stricmp(nm, fh.name)) h = &fh;
        R2Progress prog = {job, 0, size.QuadPart > 0 ? size.QuadPart : 1};
        char why[256] = "";
        if (h) {
            if (size.QuadPart > h->max_bytes) continue;
            tried++;
            const char *fields[][2] = {{"reqtype", "fileupload"}, {"time", hours}};
            wchar_t wurl[300];
            char curl[300];
            swprintf(wurl, COUNT(wurl), L"https://%s%s", h->server, h->path);
            wide_to_utf8(wurl, curl, sizeof curl);
            UpRequest q = {"POST", curl, "", fields, h->expires ? 2 : 1, "fileToUpload"};
            r2_set_progress(job, 0);
            i64 began = now_us();
            u32 status = up_send(q, f, size.QuadPart, &prog, resp, sizeof resp);
            if (status == 200 && up_plain_link(resp, url, sizeof url)) {
                used = h;
                r2_rate_note(h->host, size.QuadPart, now_us() - began);
            }
            else if (!status) snprintf(why, sizeof why, "Couldn't reach %s", h->label);
            else snprintf(why, sizeof why, "%s answered %u: %.80s", h->label, status, trim(resp));
        } else if (uploader_load(nm, &u, why, sizeof why)) {
            tried++;
            char headers[2600] = "";
            i32 hl = 0;
            for (i32 i = 0; i < u.count[0]; i++)
                hl += snprintf(headers + hl, sizeof headers - hl, "%s: %s\r\n", u.pairs[0][i][0], u.pairs[0][i][1]);
            const char *fields[UP_PAIRS][2];
            for (i32 i = 0; i < u.count[1]; i++) {
                fields[i][0] = u.pairs[1][i][0];
                fields[i][1] = u.pairs[1][i][1];
            }
            UpRequest q = {u.method, u.url, headers, fields, u.count[1], u.form_name};
            r2_set_progress(job, 0);
            u32 status = up_send(q, f, size.QuadPart, &prog, resp, sizeof resp);
            char link[512];
            if (status >= 200 && status < 300 && uploader_result(u.result, resp, link, sizeof link) &&
                starts_with(link, "http") && !strpbrk(link, " \r\n<\"")) {
                str_set(url, link);
                snprintf(used_name, sizeof used_name, "%s", nm);
            } else if (!status) snprintf(why, sizeof why, "Couldn't reach %s", u.name);
            else if (status >= 200 && status < 300) snprintf(why, sizeof why, "%s answered without a link", u.name);
            else snprintf(why, sizeof why, "%s answered %u: %.80s", u.name, status, trim(resp));
        } else tried++;
        if (!url[0] && why[0]) {
            log_line("r2: %s", why);
            str_set(err, why);
            if (*p) err[0] = 0; // the next host gets the clip
        }
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    DeleteFileW(t.file.s);
    if (!url[0] && !err[0]) {
        if (!named) str_set(err, "Turn on a free host or set up Cloudflare in Options");
        else if (!tried)
            snprintf(err, sizeof err, "Free links take clips up to %lld MB: set up Cloudflare in Options for bigger ones",
                     free_max_bytes(t.hosts) / 1000000);
        else str_set(err, g_r2.stop ? "Stopped" : "No host took the clip");
    }
    if (!url[0]) {
        r2_job_end(job, false, "", err);
        return;
    }

    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    r2_replace_locked(t.replace);
    r2_room_locked();
    R2Share &s = g_r2.shares[g_r2.count++];
    s = {};
    if (used) {
        const char *name = strrchr(url, '/');
        snprintf(s.key, sizeof s.key, "%s/%.*s", used->name, (int)(sizeof s.key - strlen(used->name) - 2), name + 1);
        s.host = used->host;
    } else {
        char rnd[17];
        r2_random(rnd, 16);
        snprintf(s.key, sizeof s.key, "%.30s/%s", used_name, rnd); // the uploader, and a name of its own
        s.host = HOST_CUSTOM;
    }
    s.version = 1;
    s.bytes = size.QuadPart;
    s.uploaded_us = now_us();
    s.expires_us = used && used->expires ? s.uploaded_us + (i64)free_hours(t.hours) * 3600 * 1000000 : 0;
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
    bool page = i >= 0 && g_r2.shares[i].page_w > 0;
    char at[R2_KEY_MAX] = "";
    if (i >= 0) str_set(at, r2_object_key(g_r2.shares[i]));
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (i < 0) return;
    if (remote && !r2_delete_share(t.conf, t.key, at, page)) {
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

// ---------------------------------------------------------------------------------------------------------------
// Cloudflare's API (api.cloudflare.com) and sign-in (dash.cloudflare.com)

// Sign in with Cloudflare: an OAuth public client with PKCE (S256), answered on a fixed loopback port. The
// redirect address and scopes are the ones the client is registered with (Account Settings Read, Workers R2 Storage
// Write, Account Analytics Read, API Tokens Write). The bucket's own token is a user API token, made with
// user-api-tokens.write.
static const char CF_OAUTH_CLIENT_ID[] = "0e0024986eb482790af86a04235cebbd";
enum { CF_OAUTH_PORT = 8977 };
static const char CF_OAUTH_REDIRECT[] = "http://127.0.0.1:8977/oauth/callback";
static const char CF_OAUTH_SCOPES[] =
    "account-settings.read workers-r2.write account-analytics.read user-api-tokens.write";
static const wchar_t CF_DASH_HOST[] = L"dash.cloudflare.com";
static const i64 CF_SIGNIN_WAIT_US = 10ll * 60 * 1000000; // how long the browser has to come back

// Permission groups: what the token link asks for, and what the bucket's own token gets (looked up by name when
// signed in, this id if that fails)
static const char CF_PERM_R2_WRITE[] = "bf7481a1826f439697cb59a20b22293e";  // Workers R2 Storage Write
static const char CF_PERM_ANALYTICS[] = "b89a480218d04ceb98b4fe57ca29dc1f"; // Account Analytics Read
static const char CF_PERM_BUCKET_ITEMS_NAME[] = "Workers R2 Storage Bucket Item Write";
static const char CF_PERM_BUCKET_ITEMS[] = "2efd5506f9c8494dacb1fa10a3e7d5b6";

// R2's error codes in Cloudflare's answers
enum { CF_NO_SUCH_BUCKET = 10006, CF_BUCKET_EXISTS = 10073, CF_R2_OFF = 10042 };

// The request classes Cloudflare bills R2 operations by: 'A', 'B', or 0 for free ones.
static char r2_op_class(const char *action)
{
    static const char *A[] = {"ListBuckets", "PutBucket", "ListObjects", "PutObject", "CopyObject",
                              "CompleteMultipartUpload", "CreateMultipartUpload", "LifecycleStorageTierTransition",
                              "ListMultipartUploads", "UploadPart", "UploadPartCopy", "ListParts",
                              "PutBucketEncryption", "PutBucketCors", "PutBucketLifecycleConfiguration"};
    static const char *B[] = {"HeadBucket", "HeadObject", "GetObject", "UsageSummary", "GetBucketEncryption",
                              "GetBucketLocation", "GetBucketCors", "GetBucketLifecycleConfiguration"};
    for (const char *a : A)
        if (!strcmp(action, a)) return 'A';
    for (const char *b : B)
        if (!strcmp(action, b)) return 'B';
    return 0;
}

// Dollars for a month with this much stored and these requests, past the free allowances.
static f64 r2_month_cost(i64 bytes, i64 class_a, i64 class_b)
{
    f64 a = (f64)class_a - R2_FREE_A, b = (f64)class_b - R2_FREE_B;
    return r2_cost_for(bytes) + (a > 0 ? a / 1e6 * R2_PRICE_A : 0) + (b > 0 ? b / 1e6 * R2_PRICE_B : 0);
}

// R2's rule for bucket names: 3 to 63 lower-case letters, digits and hyphens, a letter or digit at each end.
static bool r2_bucket_name_ok(const char *s)
{
    i32 n = (i32)strlen(s);
    if (n < 3 || n > 63 || s[0] == '-' || s[n - 1] == '-') return false;
    for (i32 i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return false;
    return true;
}

// The first error in an answer: its code (0 if none) and message.
static i32 r2_cf_error(const char *resp, char *msg, i32 cap)
{
    char code[16];
    msg[0] = 0;
    i64 len = (i64)strlen(resp);
    json_path(resp, len, "errors[0].message", msg, cap);
    return json_path(resp, len, "errors[0].code", code, sizeof code) ? atoi(code) : 0;
}

// /user/tokens/verify or /accounts/<id>/tokens/verify: the token's id, while it's active.
static bool r2_parse_verify(const char *resp, char *id, i32 cap)
{
    char status[32];
    i64 len = (i64)strlen(resp);
    return json_path(resp, len, "result.status", status, sizeof status) && !strcmp(status, "active") &&
           json_path(resp, len, "result.id", id, cap);
}

// GET /accounts: their ids and names. Returns how many.
static i32 r2_parse_accounts(const char *resp, char (*ids)[40], char (*names)[128], i32 max)
{
    i64 len = (i64)strlen(resp);
    i32 n = 0;
    for (; n < max; n++) {
        char path[48];
        snprintf(path, sizeof path, "result[%d].id", n);
        if (!json_path(resp, len, path, ids[n], 40)) break;
        snprintf(path, sizeof path, "result[%d].name", n);
        json_path(resp, len, path, names[n], 128);
    }
    return n;
}

// The bucket's r2.dev address, and whether it's on.
static bool r2_parse_domain(const char *resp, char *domain, i32 cap, bool *enabled)
{
    char on[8];
    i64 len = (i64)strlen(resp);
    *enabled = json_path(resp, len, "result.enabled", on, sizeof on) && !strcmp(on, "true");
    return json_path(resp, len, "result.domain", domain, cap);
}

// The bucket's custom domains that answer: enabled, owned and with a certificate. Returns how many.
static i32 r2_parse_custom_domains(const char *resp, char (*out)[128], i32 max)
{
    i64 len = (i64)strlen(resp);
    char path[80], v[128];
    i32 n = 0;
    for (i32 i = 0; n < max && i < 100; i++) {
        snprintf(path, sizeof path, "result.domains[%d].domain", i);
        if (!json_path(resp, len, path, out[n], 128)) break;
        bool ok = true;
        static const char *const need[][2] = {{"enabled", "true"}, {"status.ownership", "active"}, {"status.ssl", "active"}};
        for (auto &k : need) {
            snprintf(path, sizeof path, "result.domains[%d].%s", i, k[0]);
            ok = ok && json_path(resp, len, path, v, sizeof v) && !strcmp(v, k[1]);
        }
        if (ok) n++;
    }
    return n;
}

// The id of the permission group named `name` in a list of them.
static bool r2_parse_perm_group(const char *resp, const char *name, char *id, i32 cap)
{
    i64 len = (i64)strlen(resp);
    char path[48], n[128];
    for (i32 i = 0; i < 2000; i++) {
        snprintf(path, sizeof path, "result[%d].name", i);
        if (!json_path(resp, len, path, n, sizeof n)) break;
        snprintf(path, sizeof path, "result[%d].id", i);
        if (!strcmp(n, name)) return json_path(resp, len, path, id, cap);
    }
    return false;
}

// A token just made: its id and value.
static bool r2_parse_new_token(const char *resp, char *id, i32 id_cap, char *value, i32 value_cap)
{
    i64 len = (i64)strlen(resp);
    return json_path(resp, len, "result.id", id, id_cap) && json_path(resp, len, "result.value", value, value_cap);
}

struct R2OAuth {
    char access[2048], refresh[1024];
    i64  expires_in; // seconds
    char error[64];  // "invalid_grant" and the like
    char scope[512]; // what the player allowed, space separated
};

// Whether a space-separated scope list holds `name`.
static bool r2_scope_has(const char *scope, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = scope; *p;) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if ((size_t)(e - p) == n && !strncmp(p, name, n)) return true;
        p = e;
    }
    return false;
}

// Cloudflare's sign-in page lists API Tokens Write as a sensitive scope, off until the player turns it on.
#define R2_NEED_TOKENS_WRITE \
    "On Cloudflare's sign-in page, turn on \"API Tokens Write\" and sign in again: " APP_NAME " needs it to make a " \
    "key for the bucket"

// The sign-in's token answer, to the code or a refresh.
static bool r2_parse_oauth(const char *resp, R2OAuth *o)
{
    char n[24];
    i64 len = (i64)strlen(resp);
    *o = {};
    json_path(resp, len, "error", o->error, sizeof o->error);
    json_path(resp, len, "refresh_token", o->refresh, sizeof o->refresh);
    json_path(resp, len, "scope", o->scope, sizeof o->scope);
    o->expires_in = json_path(resp, len, "expires_in", n, sizeof n) ? _atoi64(n) : 0;
    return json_path(resp, len, "access_token", o->access, sizeof o->access);
}

// The GraphQL answer to r2_usage_query: what the bucket holds (its last count this month) and the requests by class.
static bool r2_parse_usage(const char *resp, R2Fetched *out)
{
    i64 len = (i64)strlen(resp);
    char v[32], action[64];
    *out = {};
    const char *acc = "data.viewer.accounts[0].";
    char path[96];
    snprintf(path, sizeof path, "%sstorage[0].max.payloadSize", acc);
    bool any = json_path(resp, len, path, v, sizeof v);
    out->bytes = any ? _atoi64(v) : 0;
    snprintf(path, sizeof path, "%sstorage[0].max.metadataSize", acc);
    if (json_path(resp, len, path, v, sizeof v)) out->bytes += _atoi64(v);
    bool has_ops = false;
    for (i32 i = 0; i < 1000; i++) {
        snprintf(path, sizeof path, "%sops[%d].dimensions.actionType", acc, i);
        if (!json_path(resp, len, path, action, sizeof action)) break;
        snprintf(path, sizeof path, "%sops[%d].sum.requests", acc, i);
        i64 n = json_path(resp, len, path, v, sizeof v) ? _atoi64(v) : 0;
        char cls = r2_op_class(action);
        if (cls == 'A') out->class_a += n;
        else if (cls == 'B') out->class_b += n;
        has_ops = true;
    }
    for (i32 i = 0; i < 1000; i++) {
        snprintf(path, sizeof path, "%saccount[%d].dimensions.actionType", acc, i);
        if (!json_path(resp, len, path, action, sizeof action)) break;
        snprintf(path, sizeof path, "%saccount[%d].sum.requests", acc, i);
        if (r2_op_class(action) == 'B' && json_path(resp, len, path, v, sizeof v)) out->account_reads += _atoi64(v);
    }
    // An empty month has empty lists; an error has no data at all
    return any || has_ops || strstr(resp, "\"storage\":[]") != 0;
}

// The GraphQL request for a bucket from `start` to `end` (ISO 8601, UTC): its storage, its requests by type, the
// GET requests for each file (plays) over that time, since `day` (local midnight, in UTC) and since `since` (24
// hours ago), and the requests by type to every bucket of the account.
static void r2_usage_query(const char *account, const char *bucket, const char *start, const char *end,
                           const char *day, const char *since, char *out, i32 cap)
{
    snprintf(out, cap,
             "{\"query\":\"query($accountTag:string!,$bucketName:string,$startDate:Time,$endDate:Time,$dayStart:Time,"
             "$since:Time,$get:string){viewer{accounts(filter:{accountTag:$accountTag}){"
             "storage:r2StorageAdaptiveGroups(limit:1,filter:{datetime_geq:$startDate,datetime_leq:$endDate,"
             "bucketName:$bucketName},orderBy:[datetime_DESC]){max{payloadSize metadataSize}} "
             "ops:r2OperationsAdaptiveGroups(limit:1000,filter:{datetime_geq:$startDate,datetime_leq:$endDate,"
             "bucketName:$bucketName}){sum{requests} dimensions{actionType}} "
             "month:r2OperationsAdaptiveGroups(limit:10000,filter:{datetime_geq:$startDate,datetime_leq:$endDate,"
             "bucketName:$bucketName,actionType:$get}){sum{requests} dimensions{objectName}} "
             "today:r2OperationsAdaptiveGroups(limit:10000,filter:{datetime_geq:$dayStart,datetime_leq:$endDate,"
             "bucketName:$bucketName,actionType:$get}){sum{requests} dimensions{objectName}} "
             "day:r2OperationsAdaptiveGroups(limit:10000,filter:{datetime_geq:$since,datetime_leq:$endDate,"
             "bucketName:$bucketName,actionType:$get}){sum{requests} dimensions{objectName}} "
             "account:r2OperationsAdaptiveGroups(limit:1000,filter:{datetime_geq:$startDate,datetime_leq:$endDate})"
             "{sum{requests} dimensions{actionType}}}}}\","
             "\"variables\":{\"accountTag\":\"%s\",\"bucketName\":\"%s\",\"startDate\":\"%s\",\"endDate\":\"%s\","
             "\"dayStart\":\"%s\",\"since\":\"%s\",\"get\":\"GetObject\"}}",
             account, bucket, start, end, day, since);
}

// One list of per-file plays in the usage answer ("month", "today" or "day"): each file's name and count. Returns how
// many.
static i32 r2_parse_plays(const char *resp, const char *list, char (*keys)[R2_KEY_MAX], i64 *counts, i32 max)
{
    i64 len = (i64)strlen(resp);
    char path[96], v[32];
    i32 n = 0;
    for (i32 i = 0; n < max && i < 10000; i++) {
        snprintf(path, sizeof path, "data.viewer.accounts[0].%s[%d].dimensions.objectName", list, i);
        char key[256];
        if (!json_path(resp, len, path, key, sizeof key)) break;
        if (strlen(key) >= R2_KEY_MAX) continue;
        snprintf(path, sizeof path, "data.viewer.accounts[0].%s[%d].sum.requests", list, i);
        str_set(keys[n], key);
        counts[n++] = json_path(resp, len, path, v, sizeof v) ? _atoi64(v) : 0;
    }
    return n;
}

// The dashboard page that makes an API token with what clipback needs already picked.
static const char *r2_token_link()
{
    static char url[600];
    if (!url[0]) {
        char keys[256], enc[512];
        snprintf(keys, sizeof keys, "[{\"key\":\"%s\",\"type\":\"edit\"},{\"key\":\"%s\",\"type\":\"read\"}]",
                 CF_PERM_R2_WRITE, CF_PERM_ANALYTICS);
        char name[64];
        r2_uri_encode(keys, enc, sizeof enc);
        r2_uri_encode(APP_NAME, name, sizeof name);
        snprintf(url, sizeof url, "https://dash.cloudflare.com/?to=/:account/api-tokens&permissionGroupKeys=%s&name=%s",
                 enc, name);
    }
    return url;
}

// base64url without padding, as PKCE uses.
static void r2_b64url(const u8 *b, i32 n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    i32 o = 0;
    for (i32 i = 0; i < n; i += 3) {
        u32 v = (u32)b[i] << 16 | (i + 1 < n ? (u32)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        if (i + 1 < n) out[o++] = T[v >> 6 & 63];
        if (i + 2 < n) out[o++] = T[v & 63];
    }
    out[o] = 0;
}

// PKCE's S256 challenge for a verifier.
static void r2_pkce_challenge(const char *verifier, char out[44])
{
    u8 h[32];
    if (r2_sha256(verifier, strlen(verifier), h)) r2_b64url(h, 32, out);
    else out[0] = 0;
}

// Undoes percent-encoding and '+' for a space.
static void r2_url_decode(const char *s, i32 n, char *out, i32 cap)
{
    i32 o = 0;
    for (i32 i = 0; i < n && o < cap - 1; i++) {
        if (s[i] == '+') out[o++] = ' ';
        else if (s[i] == '%' && i + 2 < n && isxdigit((u8)s[i + 1]) && isxdigit((u8)s[i + 2])) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            out[o++] = (char)strtol(hex, 0, 16);
            i += 2;
        } else out[o++] = s[i];
    }
    out[o] = 0;
}

// A query parameter's value, decoded. False when it isn't there.
static bool r2_query_param(const char *query, const char *name, char *out, i32 cap)
{
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = query; p && *p;) {
        const char *end = p + strcspn(p, "& ");
        if (!strncmp(p, name, nl) && p[nl] == '=') {
            r2_url_decode(p + nl + 1, (i32)(end - p - nl - 1), out, cap);
            return true;
        }
        if (*end != '&') break;
        p = end + 1;
    }
    return false;
}

enum R2Callback : u8 { R2_CB_OTHER, R2_CB_CODE, R2_CB_REFUSED, R2_CB_CANCELLED };

// What the browser sent to the loopback port: the sign-in's answer, or something else (a favicon). `request` is
// the request's first line, "GET /oauth/callback?code=...&state=... HTTP/1.1". A refusal leaves Cloudflare's
// reason in `code`.
static R2Callback r2_callback_parse(const char *request, char *code, i32 code_cap, char *state, i32 state_cap)
{
    code[0] = state[0] = 0;
    const char *p = "GET /oauth/callback?";
    if (strncmp(request, p, strlen(p))) return R2_CB_OTHER;
    const char *q = request + strlen(p);
    r2_query_param(q, "state", state, state_cap);
    char err[64];
    if (r2_query_param(q, "error", err, sizeof err)) {
        if (!r2_query_param(q, "error_description", code, code_cap)) snprintf(code, code_cap, "%s", err);
        return !strcmp(err, "access_denied") ? R2_CB_CANCELLED : R2_CB_REFUSED;
    }
    return r2_query_param(q, "code", code, code_cap) && code[0] ? R2_CB_CODE : R2_CB_OTHER;
}

// The browser's answer when the sign-in comes back: signed in, cancelled, or failed with Cloudflare's reason (made
// safe and ASCII). A request that isn't the sign-in's gets an empty 404.
static void r2_done_page(R2Callback cb, const char *reason, char *out, i32 cap)
{
    if (cb == R2_CB_OTHER) {
        snprintf(out, cap, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }
    i32 len = snprintf(out, cap,
                       "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
                       "Connection: close\r\n\r\n<!doctype html><title>" APP_NAME "</title>"
                       "<p style=\"font:16px sans-serif;margin:40px\">");
    if (cb == R2_CB_CODE)
        len += snprintf(out + len, cap - len, "Signed in. You can close this tab and go back to " APP_NAME ".");
    else if (cb == R2_CB_CANCELLED)
        len += snprintf(out + len, cap - len, "The sign-in was cancelled. You can close this tab.");
    else {
        len += snprintf(out + len, cap - len, "The sign-in failed. Cloudflare said: ");
        r2_html_text(reason, out, cap - 120, &len);
        len += snprintf(out + len, cap - len, "<br>You can close this tab and try again in " APP_NAME ".");
    }
    snprintf(out + len, cap - len, "</p>");
}

// One HTTPS request with the body in memory; `headers` are "Name: value" lines joined by \r\n. Returns the status,
// 0 = no answer; the answer's body (cut at cap) goes to resp.
static u32 r2_https(const wchar_t *host, const char *method, const char *path, const char *headers, const char *body,
                    char *resp, i32 cap)
{
    resp[0] = 0;
    wchar_t wmethod[16], wpath[1024], whdrs[4096];
    utf8_to_wide(method, wmethod, COUNT(wmethod));
    utf8_to_wide(path, wpath, COUNT(wpath));
    utf8_to_wide(headers, whdrs, COUNT(whdrs));
    DWORD blen = body ? (DWORD)strlen(body) : 0;
    u32 status = 0;
    HINTERNET session = WinHttpOpen(L"clipback", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : 0;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, wmethod, wpath, 0, 0, 0, WINHTTP_FLAG_SECURE) : 0;
    if (req) {
        WinHttpSetTimeouts(req, 15000, 15000, 30000, 60000);
        DWORD code = 0, len = sizeof code;
        if (WinHttpSendRequest(req, whdrs, (DWORD)-1L, (void *)body, blen, blen, 0) &&
            WinHttpReceiveResponse(req, 0) &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &code, &len, 0))
            status = code;
        i32 have = 0;
        while (status && cap) {
            char chunk[4096];
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk, sizeof chunk, &got) || !got) break;
            i32 take = (i32)got < cap - 1 - have ? (i32)got : cap - 1 - have;
            memcpy(resp + have, chunk, take);
            have += take;
            resp[have] = 0;
        }
        if (!status) log_line("r2: %s %ls%s: no answer (error %d)", method, host, path, (int)GetLastError());
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    SecureZeroMemory(whdrs, sizeof whdrs);
    return status;
}

// A request to Cloudflare's API with a token: `path` after /client/v4, `body` JSON or null.
static u32 r2_cf(const char *method, const char *path, const char *bearer, const char *body, char *resp, i32 cap)
{
    char full[512], hdrs[2200];
    snprintf(full, sizeof full, "/client/v4%s", path);
    snprintf(hdrs, sizeof hdrs, "Authorization: Bearer %s%s", bearer, body ? "\r\nContent-Type: application/json" : "");
    u32 s = r2_https(L"api.cloudflare.com", method, full, hdrs, body, resp, cap);
    SecureZeroMemory(hdrs, sizeof hdrs);
    if (!r2_ok(s)) DEV_LOG("r2: %s %s: %u", method, path, s);
    return s;
}

// A form post to the sign-in's /oauth2/<what>.
static u32 r2_oauth_post(const char *what, const char *form, char *resp, i32 cap)
{
    char path[64];
    snprintf(path, sizeof path, "/oauth2/%s", what);
    return r2_https(CF_DASH_HOST, "POST", path, "Content-Type: application/x-www-form-urlencoded", form, resp, cap);
}

// ---------------------------------------------------------------------------------------------------------------
// Setup, sign-in and usage (worker thread)

static bool r2_queue(const R2Task &t);

static void r2_set_status(R2SetupState state, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&g_r2.lock);
    g_r2.status.state = state;
    str_set(g_r2.status.text, text);
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (state == R2_SETUP_FAILED || state == R2_SETUP_NO_TOKEN_SCOPE) log_line("r2: setup: %s", text);
    else DEV_LOG("r2: setup: %s", text);
    r2_post(R2_EV_STATUS, state);
}

static void r2_set_who(const char *who)
{
    AcquireSRWLockExclusive(&g_r2.lock);
    str_set(g_r2.status.who, who);
    ReleaseSRWLockExclusive(&g_r2.lock);
}

// Uploads a small file, fetches it from the public address, deletes it. A token just made can take a few seconds
// to reach the bucket, so with `fresh` a refusal is tried again for a while.
static void r2_run_probe(const R2Conf &c, bool fresh)
{
    char key[17], body[64], resp[2048], why[256];
    r2_random(key, 16);
    snprintf(body, sizeof body, "clipback %s", key);
    char name[64];
    snprintf(name, sizeof name, "clipback/test-%s.txt", key);
    u32 status = 0;
    for (i32 tries = fresh ? 10 : 1; tries > 0 && !g_r2.stop; tries--) {
        status = r2_send(c, "PUT", name, "", R2Body{body, 0, 0, (i64)strlen(body)}, "text/plain", resp, sizeof resp);
        if (status != 401 && status != 403) break;
        if (tries > 1) Sleep(3000);
    }
    if (!r2_ok(status)) {
        r2_why(status, resp, c, why, sizeof why);
        r2_set_status(R2_SETUP_FAILED, "%s", why);
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
        // A new r2.dev address can take a moment to answer
        for (i32 tries = fresh ? 10 : 1; tries > 0 && !g_r2.stop; tries--) {
            char *got = 0;
            u32 answer = http_get(whost, wpath, L"", 0, &got, 0);
            bool same = answer == 200 && got && !strcmp(got, body);
            free(got);
            if (same) { fail[0] = 0; break; }
            snprintf(fail, sizeof fail,
                     answer ? "Uploaded, but %ls answered %u: turn on public access for the bucket"
                            : "Uploaded, but %ls didn't answer: check the public address",
                     whost, answer);
            if (tries > 1) Sleep(3000);
        }
    }
    bool deleted = r2_delete_object(c, name);
    if (fail[0]) r2_set_status(R2_SETUP_FAILED, "%s", fail);
    else if (!deleted) r2_set_status(R2_SETUP_FAILED, "Works, but the token can't delete from %s", c.bucket);
    else r2_set_status(R2_SETUP_OK, "Works: bucket %s, played from %ls", c.bucket, whost);
}

// A failed API answer, for the player.
static void r2_cf_fail(u32 status, const char *resp, const char *what)
{
    char msg[200];
    i32 code = r2_cf_error(resp, msg, sizeof msg);
    if (!status) r2_set_status(R2_SETUP_FAILED, "Couldn't reach Cloudflare");
    else if (code == CF_R2_OFF) r2_set_status(R2_SETUP_FAILED, "Turn on R2 in the Cloudflare dashboard first");
    else if (status == 401 || status == 403)
        r2_set_status(R2_SETUP_FAILED, "Cloudflare didn't allow %s%s%s", what, msg[0] ? ": " : "", msg);
    else r2_set_status(R2_SETUP_FAILED, "Couldn't %s: %s", what, msg[0] ? msg : "Cloudflare said no");
}

static void r2_oauth_keep(const R2OAuth &o)
{
    str_set(g_r2.access, o.access);
    g_r2.access_until_us = now_us() + (o.expires_in > 0 ? o.expires_in : 3600) * 1000000;
    if (o.refresh[0]) r2_token_store(o.refresh, true); // they're used once: keep each new one at once
}

static bool r2_oauth_refresh(const char *refresh, R2OAuth *o)
{
    static char form[2048], enc[1536], resp[16384];
    r2_uri_encode(refresh, enc, sizeof enc);
    snprintf(form, sizeof form, "grant_type=refresh_token&refresh_token=%s&client_id=%s", enc, CF_OAUTH_CLIENT_ID);
    u32 s = r2_oauth_post("token", form, resp, sizeof resp);
    bool ok = r2_parse_oauth(resp, o) && r2_ok(s);
    SecureZeroMemory(form, sizeof form);
    SecureZeroMemory(enc, sizeof enc);
    SecureZeroMemory(resp, sizeof resp);
    if (!ok) log_line("r2: couldn't refresh the sign-in (%u %s)", s, o->error);
    return ok;
}

// The sign-in's access token, refreshed when it's about to run out. False when there's no sign-in or Cloudflare
// ended it (then the sign-in is forgotten).
static bool r2_oauth_access(char *out, i32 cap)
{
    out[0] = 0;
    if (!g_r2.access[0] || now_us() > g_r2.access_until_us - 60ll * 1000000) {
        char refresh[1024];
        if (!r2_token_get(refresh, sizeof refresh, true)) return false;
        static R2OAuth o;
        bool ok = r2_oauth_refresh(refresh, &o);
        if (ok) r2_oauth_keep(o);
        else if (!strcmp(o.error, "invalid_grant")) r2_token_store("", true);
        SecureZeroMemory(refresh, sizeof refresh);
        SecureZeroMemory(&o, sizeof o);
        if (!ok) return false;
    }
    snprintf(out, cap, "%s", g_r2.access);
    return true;
}

// The steps before the probe: the account, the token's id, the bucket, its public address and, with a sign-in,
// the bucket's own token. Fills `c` (with its secret) and `token`; false once the status says what went wrong,
// with `refused` when Cloudflare wouldn't make the bucket's token.
static bool r2_setup_steps(const R2Task &t, const char *bearer, R2Conf *c, char *token, i32 token_cap, bool *fresh,
                           bool *refused)
{
    static char resp[1 << 18];
    char path[256], body[512];
    *fresh = *refused = false;
    r2_set_status(R2_SETUP_BUSY, "Checking your account...");
    u32 s = r2_cf("GET", "/accounts?per_page=50", bearer, 0, resp, sizeof resp);
    char ids[8][40], names[8][128];
    i32 n = r2_ok(s) ? r2_parse_accounts(resp, ids, names, 8) : 0;
    if (!n) {
        if (r2_ok(s)) r2_set_status(R2_SETUP_FAILED, "The token can't see any Cloudflare account");
        else r2_cf_fail(s, resp, t.oauth ? "the sign-in" : "the token");
        return false;
    }
    i32 pick = 0; // the account set up before, else the first
    for (i32 i = 0; i < n; i++)
        if (!strcmp(ids[i], t.conf.account)) pick = i;
    if (n > 1) log_line("r2: the token reaches %d accounts; using %s", n, names[pick]);
    str_set(c->account, ids[pick]);
    r2_set_who(names[pick]);

    char token_id[72] = "";
    if (!t.oauth) {
        snprintf(token, token_cap, "%s", t.token);
        s = r2_cf("GET", "/user/tokens/verify", bearer, 0, resp, sizeof resp);
        if (!r2_ok(s) || !r2_parse_verify(resp, token_id, sizeof token_id)) {
            snprintf(path, sizeof path, "/accounts/%s/tokens/verify", c->account);
            s = r2_cf("GET", path, bearer, 0, resp, sizeof resp);
            if (!r2_ok(s) || !r2_parse_verify(resp, token_id, sizeof token_id)) {
                r2_set_status(R2_SETUP_FAILED, "The token isn't active: make a new one");
                return false;
            }
        }
    }

    str_set(c->bucket, t.conf.bucket);
    if (!r2_bucket_name_ok(c->bucket)) {
        r2_set_status(R2_SETUP_FAILED, "%s can't be a bucket name: use 3 to 63 lower-case letters, digits and hyphens",
                      c->bucket);
        return false;
    }
    r2_set_status(R2_SETUP_BUSY, "Finding the bucket %s...", c->bucket);
    snprintf(path, sizeof path, "/accounts/%s/r2/buckets/%s", c->account, c->bucket);
    s = r2_cf("GET", path, bearer, 0, resp, sizeof resp);
    if (!r2_ok(s)) {
        char msg[200];
        i32 code = r2_cf_error(resp, msg, sizeof msg);
        if (s != 404 && code != CF_NO_SUCH_BUCKET) {
            r2_cf_fail(s, resp, "reading the bucket");
            return false;
        }
        r2_set_status(R2_SETUP_BUSY, "Making the bucket %s...", c->bucket);
        snprintf(path, sizeof path, "/accounts/%s/r2/buckets", c->account);
        snprintf(body, sizeof body, "{\"name\":\"%s\"}", c->bucket);
        s = r2_cf("POST", path, bearer, body, resp, sizeof resp);
        if (!r2_ok(s) && r2_cf_error(resp, msg, sizeof msg) != CF_BUCKET_EXISTS) {
            r2_cf_fail(s, resp, "making the bucket");
            return false;
        }
    }

    // The player's own domain stays; otherwise the bucket's r2.dev address, turned on
    if (t.conf.public_url[0] && !strstr(t.conf.public_url, ".r2.dev")) str_set(c->public_url, t.conf.public_url);
    else {
        r2_set_status(R2_SETUP_BUSY, "Turning on the bucket's public address...");
        snprintf(path, sizeof path, "/accounts/%s/r2/buckets/%s/domains/managed", c->account, c->bucket);
        char domain[200] = "";
        bool on = false;
        s = r2_cf("GET", path, bearer, 0, resp, sizeof resp);
        bool have = r2_ok(s) && r2_parse_domain(resp, domain, sizeof domain, &on);
        if (have && !on) {
            s = r2_cf("PUT", path, bearer, "{\"enabled\":true}", resp, sizeof resp);
            have = r2_ok(s) && r2_parse_domain(resp, domain, sizeof domain, &on) && on;
        }
        if (!have) {
            r2_cf_fail(s, resp, "turning on the public address");
            return false;
        }
        snprintf(c->public_url, sizeof c->public_url, "https://%s", domain);
    }

    if (t.oauth) {
        // The bucket's token made before, while it still works
        if (t.token[0] && t.conf.access[0]) {
            char id[72];
            s = r2_cf("GET", "/user/tokens/verify", t.token, 0, resp, sizeof resp);
            if (r2_ok(s) && r2_parse_verify(resp, id, sizeof id) && !strcmp(id, t.conf.access)) {
                snprintf(token, token_cap, "%s", t.token);
                str_set(token_id, id);
            }
        }
        if (!token[0]) {
            r2_set_status(R2_SETUP_BUSY, "Making a key for the bucket...");
            if (t.conf.access[0]) {
                snprintf(path, sizeof path, "/user/tokens/%s", t.conf.access);
                r2_cf("DELETE", path, bearer, 0, resp, sizeof resp);
            }
            char group[40] = "";
            s = r2_cf("GET", "/user/tokens/permission_groups", bearer, 0, resp, sizeof resp);
            if (!r2_ok(s) || !r2_parse_perm_group(resp, CF_PERM_BUCKET_ITEMS_NAME, group, sizeof group))
                str_set(group, CF_PERM_BUCKET_ITEMS);
            snprintf(body, sizeof body,
                     "{\"name\":\"" APP_NAME " (%s)\",\"policies\":[{\"effect\":\"allow\",\"resources\":"
                     "{\"com.cloudflare.edge.r2.bucket.%s_default_%s\":\"*\"},\"permission_groups\":[{\"id\":\"%s\"}]}]}",
                     c->bucket, c->account, c->bucket, group);
            s = r2_cf("POST", "/user/tokens", bearer, body, resp, sizeof resp);
            if (!r2_ok(s) || !r2_parse_new_token(resp, token_id, sizeof token_id, token, token_cap)) {
                char msg[200];
                r2_cf_error(resp, msg, sizeof msg);
                SecureZeroMemory(resp, sizeof resp);
                *refused = true;
                if (s == 401 || s == 403) r2_set_status(R2_SETUP_NO_TOKEN_SCOPE, R2_NEED_TOKENS_WRITE);
                else
                    r2_set_status(R2_SETUP_FAILED, "Cloudflare didn't let " APP_NAME " make a key for the bucket%s%s. "
                                  "Use a token instead.", msg[0] ? ": " : "", msg);
                return false;
            }
            SecureZeroMemory(resp, sizeof resp);
            r2_token_store(token);
            *fresh = true;
        }
    }
    str_set(c->access, token_id);
    r2_token_secret(token, c->secret);
    c->limit = t.conf.limit;
    return true;
}

// Revokes a sign-in: the bucket's token clipback made (`token_id`), then the refresh token.
static void r2_revoke(const char *refresh, const char *token_id)
{
    char bearer[2048] = "";
    static R2OAuth o;
    if (g_r2.access[0] && now_us() < g_r2.access_until_us) str_set(bearer, g_r2.access);
    else if (refresh[0] && r2_oauth_refresh(refresh, &o)) {
        str_set(bearer, o.access);
        refresh = o.refresh[0] ? o.refresh : refresh;
    }
    char path[128], resp[4096];
    if (bearer[0] && token_id[0]) {
        snprintf(path, sizeof path, "/user/tokens/%s", token_id);
        u32 s = r2_cf("DELETE", path, bearer, 0, resp, sizeof resp);
        if (!r2_ok(s)) log_line("r2: couldn't delete the bucket's token (%u)", s);
    }
    if (refresh[0]) {
        static char form[2048], enc[1536];
        r2_uri_encode(refresh, enc, sizeof enc);
        snprintf(form, sizeof form, "token=%s&token_type_hint=refresh_token&client_id=%s", enc, CF_OAUTH_CLIENT_ID);
        u32 s = r2_oauth_post("revoke", form, resp, sizeof resp);
        if (!r2_ok(s)) log_line("r2: couldn't revoke the sign-in (%u)", s);
        SecureZeroMemory(form, sizeof form);
        SecureZeroMemory(enc, sizeof enc);
    }
    SecureZeroMemory(bearer, sizeof bearer);
    SecureZeroMemory(&o, sizeof o);
    SecureZeroMemory(g_r2.access, sizeof g_r2.access);
    g_r2.access_until_us = 0;
}

// Setup, then the probe. When Cloudflare won't make the bucket's token, the sign-in is no use and is revoked.
static void r2_run_setup(const R2Task &t, const char *bearer)
{
    static R2Conf c;
    char token[256] = "";
    bool fresh = false, refused = false;
    c = {};
    if (r2_setup_steps(t, bearer, &c, token, sizeof token, &fresh, &refused)) {
        AcquireSRWLockExclusive(&g_r2.lock);
        g_r2.found = c;
        SecureZeroMemory(g_r2.found.secret, sizeof g_r2.found.secret);
        ReleaseSRWLockExclusive(&g_r2.lock);
        r2_post(R2_EV_SETUP, 0);
        r2_set_status(R2_SETUP_BUSY, "Testing...");
        r2_run_probe(c, fresh);
    } else if (refused) {
        char refresh[1024];
        if (r2_token_get(refresh, sizeof refresh, true)) r2_revoke(refresh, "");
        r2_token_store("", true);
        SecureZeroMemory(refresh, sizeof refresh);
    }
    SecureZeroMemory(token, sizeof token);
    SecureZeroMemory(&c, sizeof c);
}

// Trades the browser's code for the sign-in's tokens, then sets up.
static void r2_run_signin(const R2Task &t)
{
    r2_set_status(R2_SETUP_BUSY, "Signing in...");
    static char form[3072], code[1536], redirect[128], resp[16384];
    static R2OAuth o;
    r2_uri_encode(t.code, code, sizeof code);
    r2_uri_encode(CF_OAUTH_REDIRECT, redirect, sizeof redirect);
    snprintf(form, sizeof form,
             "grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&code_verifier=%s", code, redirect,
             CF_OAUTH_CLIENT_ID, t.verifier);
    u32 s = r2_oauth_post("token", form, resp, sizeof resp);
    bool ok = r2_parse_oauth(resp, &o) && r2_ok(s);
    SecureZeroMemory(form, sizeof form);
    SecureZeroMemory(code, sizeof code);
    SecureZeroMemory(resp, sizeof resp);
    if (!ok) {
        if (!s) r2_set_status(R2_SETUP_FAILED, "Couldn't reach Cloudflare");
        else r2_set_status(R2_SETUP_FAILED, "Cloudflare didn't finish the sign-in%s%s", o.error[0] ? ": " : "",
                           o.error);
    } else if (o.scope[0] && !r2_scope_has(o.scope, "user-api-tokens.write")) {
        log_line("r2: the sign-in came without user-api-tokens.write (%s)", o.scope);
        r2_set_status(R2_SETUP_NO_TOKEN_SCOPE, R2_NEED_TOKENS_WRITE);
    } else {
        if (!o.refresh[0]) log_line("r2: the sign-in came without a refresh token");
        r2_oauth_keep(o);
        R2Task setup = t;
        setup.oauth = true;
        r2_run_setup(setup, o.access);
        SecureZeroMemory(&setup, sizeof setup);
    }
    SecureZeroMemory(&o, sizeof o);
}

// The bucket's storage and requests this month, from Cloudflare's analytics; and the account's name when it isn't
// known yet.
static void r2_run_usage(const R2Task &t)
{
    char bearer[2048];
    if (t.oauth ? !r2_oauth_access(bearer, sizeof bearer) : !t.token[0]) return;
    if (!t.oauth) str_set(bearer, t.token);
    SYSTEMTIME st;
    GetSystemTime(&st);
    char start[32], end[32], day[32], since[32], body[3200];
    snprintf(start, sizeof start, "%04d-%02d-01T00:00:00Z", st.wYear, st.wMonth);
    snprintf(end, sizeof end, "%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
    SYSTEMTIME local, midnight;
    GetLocalTime(&local);
    local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
    if (!TzSpecificLocalTimeToSystemTime(0, &local, &midnight)) midnight = st;
    snprintf(day, sizeof day, "%04d-%02d-%02dT%02d:%02d:%02dZ", midnight.wYear, midnight.wMonth, midnight.wDay,
             midnight.wHour, midnight.wMinute, midnight.wSecond);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER back = {{ft.dwLowDateTime, ft.dwHighDateTime}};
    back.QuadPart -= 24ull * 3600 * 10000000;
    ft = {back.LowPart, back.HighPart};
    SYSTEMTIME ago;
    FileTimeToSystemTime(&ft, &ago);
    snprintf(since, sizeof since, "%04d-%02d-%02dT%02d:%02d:%02dZ", ago.wYear, ago.wMonth, ago.wDay, ago.wHour,
             ago.wMinute, ago.wSecond);
    r2_usage_query(t.conf.account, t.conf.bucket, start, end, day, since, body, sizeof body);
    static char resp[1 << 20];
    u32 s = r2_cf("POST", "/graphql", bearer, body, resp, sizeof resp);
    R2Fetched f;
    if (r2_ok(s) && r2_parse_usage(resp, &f)) {
        static char keys[R2_MAX_SHARES][R2_KEY_MAX];
        static i64 counts[R2_MAX_SHARES];
        AcquireSRWLockExclusive(&g_r2.lock);
        g_r2.fetched = f;
        g_r2.usage_us = now_us();
        r2_load_locked();
        for (i32 i = 0; i < g_r2.count; i++) {
            R2Share &sh = g_r2.shares[i];
            sh.plays_today = sh.plays_month = sh.plays_24h = sh.host == HOST_R2 ? 0 : -1;
        }
        static const char *lists[] = {"month", "today", "day"};
        for (i32 pass = 0; pass < 3; pass++) {
            i32 n = r2_parse_plays(resp, lists[pass], keys, counts, R2_MAX_SHARES);
            for (i32 i = 0; i < n; i++) {
                i32 k = r2_find_object_locked(keys[i]);
                if (k < 0) continue;
                R2Share &sh = g_r2.shares[k];
                (pass == 0 ? sh.plays_month : pass == 1 ? sh.plays_today : sh.plays_24h) += counts[i];
            }
        }
        g_r2.gen++;
        ReleaseSRWLockExclusive(&g_r2.lock);
        r2_post(R2_EV_USAGE, 0);
    } else log_line("r2: couldn't read the bucket's usage (%u)", s);

    AcquireSRWLockShared(&g_r2.lock);
    bool named = g_r2.status.who[0] != 0;
    ReleaseSRWLockShared(&g_r2.lock);
    char path[96], name[128];
    snprintf(path, sizeof path, "/accounts/%s", t.conf.account);
    if (!named && r2_ok(r2_cf("GET", path, bearer, 0, resp, sizeof resp)) &&
        json_path(resp, (i64)strlen(resp), "result.name", name, sizeof name)) {
        r2_set_who(name);
        r2_post(R2_EV_STATUS, 0);
    }
    SecureZeroMemory(bearer, sizeof bearer);
}

// Lists the addresses the bucket answers at: its r2.dev domain and the custom domains connected to it.
static void r2_run_domains(const R2Task &t)
{
    char bearer[2048] = "";
    bool have = t.oauth ? r2_oauth_access(bearer, sizeof bearer) : t.token[0] != 0;
    if (!t.oauth && have) str_set(bearer, t.token);
    static char resp[1 << 16];
    static R2Domains d;
    d = {};
    d.state = -1;
    if (!have) str_set(d.why, APP_NAME " isn't signed in");
    else {
        char path[200];
        snprintf(path, sizeof path, "/accounts/%s/r2/buckets/%s/domains/managed", t.conf.account, t.conf.bucket);
        if (r2_ok(r2_cf("GET", path, bearer, 0, resp, sizeof resp)))
            r2_parse_domain(resp, d.r2dev, sizeof d.r2dev, &d.r2dev_on);
        snprintf(path, sizeof path, "/accounts/%s/r2/buckets/%s/domains/custom", t.conf.account, t.conf.bucket);
        u32 s = r2_cf("GET", path, bearer, 0, resp, sizeof resp);
        if (r2_ok(s)) {
            d.count = r2_parse_custom_domains(resp, d.list, COUNT(d.list));
            d.state = 2;
        } else {
            char msg[200] = "";
            if (s) r2_cf_error(resp, msg, sizeof msg);
            if (s == 401 || s == 403) str_set(d.why, "the sign-in or token isn't allowed to");
            else str_set(d.why, msg[0] ? msg : "Cloudflare didn't answer");
            log_line("r2: couldn't list the bucket's domains (%u): %s", s, msg);
        }
    }
    SecureZeroMemory(bearer, sizeof bearer);
    AcquireSRWLockExclusive(&g_r2.lock);
    d.asked_us = g_r2.domains.asked_us;
    g_r2.domains = d;
    ReleaseSRWLockExclusive(&g_r2.lock);
    r2_post(R2_EV_STATUS, 0);
}

// Pauses or resumes a share by moving its file in the bucket: a server-side copy to the other name, then a delete
// of the first. Paused, the file is under a new random name, so the link stops answering; a preview page's picture
// moves with it, and the page is written again without the clip (paused) or with it.
static void r2_run_move(const R2Task &t)
{
    bool pause = t.kind == R2_T_PAUSE;
    char from[R2_KEY_MAX] = "", to[R2_KEY_MAX] = "";
    R2Share share = {};
    AcquireSRWLockExclusive(&g_r2.lock);
    i32 i = r2_find_locked(t.key);
    bool go = i >= 0 && r2_in_bucket(g_r2.shares[i]) && (g_r2.shares[i].paused != 0) != pause;
    if (go) share = g_r2.shares[i];
    if (go && pause) {
        str_set(from, g_r2.shares[i].key);
        r2_new_key(to);
    } else if (go) {
        str_set(from, g_r2.shares[i].hidden);
        str_set(to, g_r2.shares[i].key);
    }
    if (i >= 0 && !go) g_r2.shares[i].moving = false;
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (!go) {
        r2_post(R2_EV_HISTORY, 0);
        return;
    }
    char resp[2048], why[256] = "";
    u32 status = r2_send(t.conf, "PUT", to, "", R2Body{}, 0, resp, sizeof resp, 0, 0, 0, from);
    bool ok = r2_ok(status) && !strstr(resp, "<Error>"); // a copy can fail with a 200 and an error body
    if (ok && !r2_delete_object(t.conf, from)) {
        r2_delete_object(t.conf, to); // the first is still there: as it was
        ok = false;
        snprintf(why, sizeof why, "the token can't delete from %s", t.conf.bucket);
    } else if (!ok) r2_why(status, resp, t.conf, why, sizeof why);
    if (ok && share.page_w) { // the picture goes with the file; the page stops or starts playing it
        char pic_from[R2_KEY_MAX], pic_to[R2_KEY_MAX];
        r2_thumb_key(from, pic_from);
        r2_thumb_key(to, pic_to);
        status = r2_send(t.conf, "PUT", pic_to, "", R2Body{}, 0, resp, sizeof resp, 0, 0, 0, pic_from);
        if (r2_ok(status) && !strstr(resp, "<Error>")) r2_delete_object(t.conf, pic_from);
        if (!r2_put_page(t.conf, share, pause)) log_line("r2: couldn't rewrite the page of %s", t.key);
    }

    AcquireSRWLockExclusive(&g_r2.lock);
    i = r2_find_locked(t.key);
    if (i >= 0) {
        R2Share &sh = g_r2.shares[i];
        sh.moving = false;
        if (ok) {
            sh.paused = pause ? t.pause : 0;
            str_set(sh.hidden, pause ? to : "");
            if (t.pause == 2 || (!pause && t.pause == 1)) sh.pause_day = r2_today();
        }
        r2_save_locked();
    }
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (ok)
        log_line("r2: %s %s%s", pause ? "paused" : "resumed", t.key,
                 !pause ? "" : t.pause == 2 ? " (too many reads in a day)" : t.pause == 3 ? " (the month's reads)" : "");
    else {
        log_line("r2: couldn't %s %s: %s", pause ? "pause" : "resume", t.key, why);
        char text[300];
        snprintf(text, sizeof text, "Couldn't %s the link: %s", pause ? "pause" : "resume", why);
        event_post_text(EV_LOG, text);
    }
    r2_post(R2_EV_HISTORY, 0);
}

// The loopback listener: waits for the browser to come back from Cloudflare's sign-in page, answers it with a
// page that says it can be closed, and queues the sign-in task with the code.
static DWORD WINAPI r2_signin_thread(void *)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(CF_OAUTH_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL excl = TRUE;
    if (ls != INVALID_SOCKET) setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
    if (ls == INVALID_SOCKET || bind(ls, (sockaddr *)&a, sizeof a) || listen(ls, 4)) {
        r2_set_status(R2_SETUP_FAILED, "Another program is using port %d: close it and sign in again", CF_OAUTH_PORT);
        if (ls != INVALID_SOCKET) closesocket(ls);
        WSACleanup();
        return 0;
    }
    i64 deadline = now_us() + CF_SIGNIN_WAIT_US;
    while (!g_r2.signin.cancel && !g_r2.stop) {
        if (now_us() > deadline) {
            r2_set_status(R2_SETUP_FAILED, "The sign-in took too long: try again");
            break;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ls, &fds);
        timeval tv = {0, 250000};
        if (select(0, &fds, 0, 0, &tv) <= 0) continue;
        SOCKET s = accept(ls, 0, 0);
        if (s == INVALID_SOCKET) continue;
        DWORD wait_ms = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&wait_ms, sizeof wait_ms);
        static char req[8192];
        i32 have = 0;
        req[0] = 0;
        while (have < (i32)sizeof req - 1) {
            int got = recv(s, req + have, (int)sizeof req - 1 - have, 0);
            if (got <= 0) break;
            have += got;
            req[have] = 0;
            if (strstr(req, "\r\n\r\n")) break;
        }
        char code[1024], state[64];
        R2Callback cb = r2_callback_parse(req, code, sizeof code, state, sizeof state);
        bool ours = cb != R2_CB_OTHER && !strcmp(state, g_r2.signin.state);
        char out[2048];
        r2_done_page(ours ? cb : R2_CB_OTHER, code, out, sizeof out);
        send(s, out, (int)strlen(out), 0);
        shutdown(s, SD_SEND);
        closesocket(s);
        SecureZeroMemory(req, sizeof req);
        if (!ours) continue;
        if (cb == R2_CB_REFUSED) r2_set_status(R2_SETUP_FAILED, "Cloudflare didn't sign in: %s", code);
        else if (cb == R2_CB_CANCELLED) r2_set_status(R2_SETUP_FAILED, "The sign-in was cancelled");
        else {
            static R2Task t;
            t = g_r2.signin.task;
            str_set(t.code, code);
            r2_set_status(R2_SETUP_BUSY, "Signing in...");
            if (!r2_queue(t)) r2_set_status(R2_SETUP_FAILED, "Busy: try again in a moment");
            SecureZeroMemory(&t, sizeof t);
        }
        SecureZeroMemory(code, sizeof code);
        break;
    }
    closesocket(ls);
    WSACleanup();
    return 0;
}

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
        case R2_T_SETUP: {
            static char bearer[2048];
            if (!t.oauth) r2_run_setup(t, t.token);
            else if (r2_oauth_access(bearer, sizeof bearer)) r2_run_setup(t, bearer);
            else r2_set_status(R2_SETUP_FAILED, "Cloudflare ended the sign-in: sign in again");
            SecureZeroMemory(bearer, sizeof bearer);
        } break;
        case R2_T_SIGNIN: r2_run_signin(t); break;
        case R2_T_USAGE: r2_run_usage(t); break;
        case R2_T_SIGNOUT: r2_revoke(t.token, t.conf.access); break;
        case R2_T_PAUSE:
        case R2_T_RESUME: r2_run_move(t); break;
        case R2_T_DOMAINS: r2_run_domains(t); break;
        case R2_T_CLEANUP: r2_run_cleanup(t); break;
        }
        SecureZeroMemory(t.conf.secret, sizeof t.conf.secret);
        SecureZeroMemory(t.token, sizeof t.token);
        SecureZeroMemory(t.code, sizeof t.code);
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

// The Space bar: the bucket's links oldest first, with goes[i] = 1 for those cleared to fit `limit` with
// `incoming` more bytes (r2_evict_plan). Returns how many.
static i32 r2_evict_preview(i64 limit, i64 incoming, u8 *goes, R2Share *out, i32 max)
{
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    i32 n = r2_evict_plan(g_r2.shares, g_r2.count, limit, incoming, r2_untracked_locked(g_r2.folder_bytes), goes,
                          out, max);
    ReleaseSRWLockExclusive(&g_r2.lock);
    return n;
}

// What the bucket's clipback/ folder holds that no link counts, from its last listing (0 before one).
static i64 r2_untracked_bytes()
{
    AcquireSRWLockShared(&g_r2.lock);
    i64 n = r2_untracked_locked(g_r2.folder_bytes);
    ReleaseSRWLockShared(&g_r2.lock);
    return n;
}

// Options' Save, after writing r2_budget: the new budget is in force from now, and the oldest unpinned links are
// cleared on the worker until the bucket fits it.
static void r2_cleanup_now()
{
    g_r2.budget = g_app.config.r2_budget;
    g_r2.budget_known = true;
    if (!r2_ready()) return;
    static R2Task t;
    t = {};
    t.kind = R2_T_CLEANUP;
    if (r2_conf(&t.conf)) r2_queue(t);
    SecureZeroMemory(&t, sizeof t);
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

// r2_refuses with the keys and reads as they are now.
static const char *r2_bucket_refuses()
{
    R2Conf c;
    bool keys = r2_conf(&c);
    SecureZeroMemory(&c, sizeof c);
    AcquireSRWLockShared(&g_r2.lock);
    bool capped = g_r2.usage_us && r2_capped(g_r2.fetched.account_reads, g_app.config.r2_read_cap);
    ReleaseSRWLockShared(&g_r2.lock);
    return r2_refuses(keys, capped);
}

// Share's host rule for a link with no host given (share.cpp): where it goes and the copy's sizes, else why not.
static bool share_link_plan(const wchar_t *path, const MediaInfo &m, const EditRecord *edit, char *hosts, i32 cap,
                            i64 *aim, i64 *limit, char *why, i32 why_cap);

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
    char why[160];
    static const EditRecord none = {};
    const EditRecord *edit = memcmp(&w.edit, &none, sizeof none) ? &w.edit : 0;
    if (!w.hosts[0] && !share_link_plan(w.src.s, w.info, edit, w.hosts, sizeof w.hosts, &w.aim, &w.limit, why,
                                        sizeof why)) {
        r2_fail_now(slot, why);
        return;
    }
    if (!strcmp(w.hosts, "r2"))
        if (const char *no = r2_bucket_refuses()) {
            r2_fail_now(slot, no);
            return;
        }
    AcquireSRWLockExclusive(&g_r2.lock);
    g_r2.jobs[slot].state = R2_PREPARING;
    ReleaseSRWLockExclusive(&g_r2.lock);
    w.save = save_link(w.src, w.info, w.edit, stem, w.aim, w.limit);
    if (!w.save) r2_fail_now(slot, "Too many saves are waiting");
}

// Shares a file as a link, to R2 when it's set up, else to a free host: `info` and `edit` when the caller has them
// (an editor render passes its edit; with no info the file is probed first). A library item already shared gives
// back its link at once while that still plays; one removed to make room is shared again under its old name, one a
// free host dropped gets a new link. `hosts` is where Share decided it goes, with the copy made for `aim` bytes and
// at most `limit` (save_link); with no hosts Share's rule decides once the file is probed. Follow the job with r2_job(id); EV_R2 says
// when it moves on. Returns the job id, 0 if every job slot is busy.
static u32 r2_share_file(const wchar_t *path, const char *title, const char *game, u64 lib_key,
                         const MediaInfo *info = 0, const EditRecord *edit = 0, const char *hosts = 0, i64 aim = 0,
                         i64 limit = 0)
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
        char url[512] = "", resume[R2_KEY_MAX] = "";
        if (found >= 0 && r2_share_live(g_r2.shares[found], now_us())) str_set(url, g_r2.shares[found].url);
        else if (found >= 0 && g_r2.shares[found].paused && !g_r2.shares[found].removed) {
            str_set(url, g_r2.shares[found].url); // a paused one is brought back
            str_set(resume, g_r2.shares[found].key);
        } else if (found >= 0) str_set(w.reuse, g_r2.shares[found].key);
        ReleaseSRWLockExclusive(&g_r2.lock);
        if (resume[0]) r2_resume(resume);
        if (url[0]) {
            u32 id = g_r2.jobs[slot].id;
            r2_job_end(id, true, url, "");
            return id;
        }
    }
    if (hosts && !strcmp(hosts, "r2"))
        if (const char *why = r2_bucket_refuses()) return r2_fail_now(slot, why);
    if (hosts) str_set(w.hosts, hosts);
    w.aim = aim;
    w.limit = limit;
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

// Stops a link while its copy is being made; an upload under way finishes.
static void r2_cancel(u32 id)
{
    for (i32 i = 0; i < COUNT(g_r2.jobs); i++)
        if (id && g_r2.jobs[i].id == id && g_r2.work[i].save) save_cancel(g_r2.work[i].save);
}

// Whether a link made here still plays (not removed, paused or expired).
static bool r2_url_live(const char *url)
{
    bool live = false;
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    for (i32 i = 0; i < g_r2.count; i++)
        if (!strcmp(g_r2.shares[i].url, url)) live = r2_share_live(g_r2.shares[i], now_us());
    ReleaseSRWLockExclusive(&g_r2.lock);
    return live;
}

static bool r2_holds_save(u32 id)
{
    for (i32 i = 0; i < COUNT(g_r2.work); i++)
        if (id && g_r2.work[i].save == id) return true;
    return false;
}

static void share_poll(); // share.cpp

static void r2_on_save_event(const Event &e)
{
    share_poll(); // Share's files are saves too
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
    t.slot = slot;
    utf8_to_wide(v->path, t.file.s, MAX_PATH);
    bool to_r2 = !strcmp(w.hosts, "r2");
    if (const char *why = to_r2 ? r2_bucket_refuses() : 0) {
        DeleteFileW(t.file.s);
        r2_fail_now(slot, why);
        return;
    }
    r2_conf(&t.conf);
    t.kind = to_r2 ? R2_T_UPLOAD : R2_T_FREE;
    str_set(t.replace, w.reuse);
    AcquireSRWLockShared(&g_r2.lock);
    i32 old = w.reuse[0] ? r2_find_locked(w.reuse) : -1;
    if (t.kind == R2_T_UPLOAD && old >= 0 && g_r2.shares[old].host == HOST_R2) str_set(t.key, w.reuse);
    ReleaseSRWLockShared(&g_r2.lock);
    str_set(t.hosts, w.hosts);
    t.hours = g_app.config.litterbox_hours;
    t.page = to_r2 && g_app.config.share_link_page && ffmpeg_find(g_app.config.ffmpeg, &t.ffmpeg);
    if (!r2_queue(t)) {
        DeleteFileW(t.file.s);
        r2_fail_now(slot, "Too many links are waiting");
    }
}

static void r2_on_event(const Event &e)
{
    share_poll();
    if (e.a == R2_EV_SETUP) {
        // What the setup found goes to the settings
        AcquireSRWLockExclusive(&g_r2.lock);
        R2Conf f = g_r2.found;
        g_r2.fetched = {};
        g_r2.usage_us = g_r2.usage_asked_us = 0;
        g_r2.domains = {};
        ReleaseSRWLockExclusive(&g_r2.lock);
        Config &c = g_app.config;
        str_set(c.r2_account, f.account);
        str_set(c.r2_access_key, f.access);
        str_set(c.r2_bucket, f.bucket);
        str_set(c.r2_public_url, f.public_url);
        config_save(c, g_app.paths);
        log_line("r2: set up: bucket %s at %s", f.bucket, f.public_url);
        return;
    }
    if (e.a == R2_EV_USAGE) {
        r2_auto_pause();
        return;
    }
    if (e.a == R2_EV_RATE) {
        Config &c = g_app.config;
        i32 host = (i32)(e.b & 3);
        r2_rate_add(&c.upload_rate[host], &c.upload_rate_n[host], (f64)(e.b >> 2));
        config_save(c, g_app.paths);
        return;
    }
    if (e.a != R2_EV_PROBED) return; // the rest only wake the UI
    i32 slot = (i32)e.b;
    if (slot < 0 || slot >= COUNT(g_r2.work)) return;
    if (!g_r2.work[slot].has_info) r2_fail_now(slot, "Couldn't read the file");
    else r2_start_copy(slot);
}

// Queues a pause (how: 1 by hand, 2 too many reads in a day, 3 the month's reads) or a resume (how: 1 by hand, 0
// automatic).
static void r2_move(const char *key, bool pause, u8 how)
{
    static R2Task t;
    t = {};
    t.kind = pause ? R2_T_PAUSE : R2_T_RESUME;
    t.pause = how;
    str_set(t.key, key);
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    i32 i = r2_find_locked(key);
    bool go = i >= 0 && r2_in_bucket(g_r2.shares[i]) && !g_r2.shares[i].moving &&
              (g_r2.shares[i].paused != 0) != pause;
    if (go) {
        g_r2.shares[i].moving = true;
        g_r2.gen++;
    }
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (!go) return;
    if (!r2_conf(&t.conf) || !r2_queue(t)) {
        AcquireSRWLockExclusive(&g_r2.lock);
        i = r2_find_locked(key);
        if (i >= 0) g_r2.shares[i].moving = false;
        ReleaseSRWLockExclusive(&g_r2.lock);
        log_line("r2: couldn't %s %s now", pause ? "pause" : "resume", key);
    }
    SecureZeroMemory(&t, sizeof t);
}

// Pauses an R2 link: it stops answering until resumed. The file stays in the bucket (and in the budget).
static void r2_pause(const char *key) { r2_move(key, true, 1); }

static void r2_resume(const char *key) { r2_move(key, false, 1); }

// After new reads came: pauses links read too often and, near the month's cap, every link, busiest first; brings
// back the ones whose reason has passed. What doesn't fit the task queue is done at the next read.
static void r2_auto_pause()
{
    struct Act { char key[R2_KEY_MAX]; i32 a; i64 month; };
    static Act acts[R2_MAX_SHARES];
    const Config &c = g_app.config;
    i32 n = 0, today = r2_today();
    AcquireSRWLockShared(&g_r2.lock);
    bool capped = g_r2.usage_us && r2_capped(g_r2.fetched.account_reads, c.r2_read_cap);
    for (i32 i = 0; i < g_r2.count; i++) {
        const R2Share &s = g_r2.shares[i];
        i32 a = r2_auto_action(s, c.r2_read_rate, capped, today);
        if (!a) continue;
        str_set(acts[n].key, s.key);
        acts[n].a = a;
        acts[n++].month = s.plays_month;
    }
    ReleaseSRWLockShared(&g_r2.lock);
    qsort(acts, n, sizeof acts[0], [](const void *x, const void *y) {
        i64 a = ((const Act *)x)->month, b = ((const Act *)y)->month;
        return a > b ? -1 : a < b ? 1 : 0;
    });
    for (i32 i = 0; i < n; i++) r2_move(acts[i].key, acts[i].a > 0, (u8)(acts[i].a > 0 ? acts[i].a : 0));
}

// Every second from the main loop: keeps the hourly read of the analytics going while no page asks for it, so the
// read limits work with the window closed.
static void r2_tick()
{
    static i64 next;
    i64 now = now_us();
    if (now < next) return;
    next = now + 60ll * 1000000;
    if (r2_ready()) r2_usage();
}

static void r2_status_set_now(R2SetupState state, const char *text)
{
    AcquireSRWLockExclusive(&g_r2.lock);
    g_r2.status.state = state;
    str_set(g_r2.status.text, text);
    ReleaseSRWLockExclusive(&g_r2.lock);
}

static R2Status r2_status()
{
    AcquireSRWLockShared(&g_r2.lock);
    R2Status s = g_r2.status;
    ReleaseSRWLockShared(&g_r2.lock);
    return s;
}

// A setup task with the settings as they are now and the API token kept.
static void r2_setup_task(R2Task *t, R2TaskKind kind)
{
    *t = {};
    t->kind = kind;
    r2_conf(&t->conf);
    SecureZeroMemory(t->conf.secret, sizeof t->conf.secret);
    r2_token_get(t->token, sizeof t->token);
    t->oauth = kind == R2_T_SIGNIN || r2_signed_in();
}

// The bucket's addresses as last listed. `ask` lists them again when that was over 5 minutes ago.
static R2Domains r2_domains(bool ask)
{
    i64 now = now_us();
    bool go = false;
    AcquireSRWLockExclusive(&g_r2.lock);
    if (ask && g_r2.domains.state != 1 && (!g_r2.domains.asked_us || now - g_r2.domains.asked_us > 300ll * 1000000)) {
        g_r2.domains.asked_us = now;
        g_r2.domains.state = 1; // the list so far stays shown
        go = true;
    }
    R2Domains d = g_r2.domains;
    ReleaseSRWLockExclusive(&g_r2.lock);
    if (go) {
        static R2Task t;
        r2_setup_task(&t, R2_T_DOMAINS);
        if (!r2_queue(t)) {
            AcquireSRWLockExclusive(&g_r2.lock);
            g_r2.domains.state = 0;
            ReleaseSRWLockExclusive(&g_r2.lock);
        }
        SecureZeroMemory(&t, sizeof t);
    }
    return d;
}

// Sets up again with what's kept (the sign-in or the pasted token): after the bucket or address changes.
static void r2_setup_again()
{
    static R2Task t;
    r2_setup_task(&t, R2_T_SETUP);
    if (!t.oauth && !t.token[0]) r2_status_set_now(R2_SETUP_IDLE, "");
    else {
        r2_status_set_now(R2_SETUP_BUSY, "Starting...");
        if (!r2_queue(t)) r2_status_set_now(R2_SETUP_FAILED, "Busy: try again in a moment");
    }
    SecureZeroMemory(&t, sizeof t);
}

// Stops waiting for the browser.
static void r2_sign_in_cancel()
{
    if (!g_r2.signin.thread) return;
    InterlockedExchange(&g_r2.signin.cancel, 1);
    WaitForSingleObject(g_r2.signin.thread, 2000);
    CloseHandle(g_r2.signin.thread);
    g_r2.signin.thread = 0;
    SecureZeroMemory(&g_r2.signin.task, sizeof g_r2.signin.task);
    if (r2_status().state == R2_SETUP_WAITING) r2_status_set_now(R2_SETUP_IDLE, "");
}

// Opens Cloudflare's sign-in page in the browser and waits for it to come back to CF_OAUTH_REDIRECT.
static void r2_sign_in()
{
    r2_sign_in_cancel();
    u8 raw[32];
    BCryptGenRandom(0, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    r2_setup_task(&g_r2.signin.task, R2_T_SIGNIN);
    r2_b64url(raw, 32, g_r2.signin.task.verifier);
    SecureZeroMemory(raw, sizeof raw);
    r2_random(g_r2.signin.state, 32);
    char challenge[44], redirect[128], scopes[256], url[1024];
    r2_pkce_challenge(g_r2.signin.task.verifier, challenge);
    r2_uri_encode(CF_OAUTH_REDIRECT, redirect, sizeof redirect);
    r2_uri_encode(CF_OAUTH_SCOPES, scopes, sizeof scopes);
    snprintf(url, sizeof url,
             "https://dash.cloudflare.com/oauth2/auth?response_type=code&client_id=%s&redirect_uri=%s&scope=%s"
             "&state=%s&code_challenge=%s&code_challenge_method=S256",
             CF_OAUTH_CLIENT_ID, redirect, scopes, g_r2.signin.state, challenge);
    InterlockedExchange(&g_r2.signin.cancel, 0);
    g_r2.signin.thread = thread_start(r2_signin_thread, 0);
    r2_status_set_now(R2_SETUP_WAITING, "Waiting for Cloudflare in the browser...");
    log_line("r2: signing in with Cloudflare");
    open_url(url);
}

// Forgets the setup: the settings it made, the tokens, and the usage read. A sign-in is revoked at Cloudflare,
// with the bucket's token clipback made; a pasted token stays the player's.
static void r2_forget(bool revoke)
{
    r2_sign_in_cancel();
    Config &c = g_app.config;
    static R2Task t;
    t = {};
    if (revoke && r2_token_get(t.token, sizeof t.token, true)) {
        t.kind = R2_T_SIGNOUT;
        str_set(t.conf.account, c.r2_account);
        str_set(t.conf.access, c.r2_access_key);
        if (!r2_queue(t)) log_line("r2: too many tasks waiting; the sign-in wasn't revoked");
    }
    SecureZeroMemory(&t, sizeof t);
    r2_token_store("", true);
    r2_token_store("");
    c.r2_account[0] = c.r2_access_key[0] = 0;
    if (strstr(c.r2_public_url, ".r2.dev")) c.r2_public_url[0] = 0; // the player's own domain stays
    config_save(c, g_app.paths);
    AcquireSRWLockExclusive(&g_r2.lock);
    g_r2.fetched = {};
    g_r2.usage_us = g_r2.usage_asked_us = 0;
    g_r2.status = {};
    g_r2.domains = {};
    ReleaseSRWLockExclusive(&g_r2.lock);
}

// Signs out of Cloudflare, or forgets a pasted token.
static void r2_sign_out()
{
    log_line("r2: signed out");
    r2_forget(true);
}

// Keeps a pasted API token and sets up with it; "" forgets it. A sign-in gives way to it.
static void r2_set_token(const char *token)
{
    if (r2_signed_in() || !token || !token[0]) r2_forget(true);
    if (!token || !token[0]) return;
    r2_token_store(token);
    r2_setup_again();
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

// What the links take up and cost. With R2 set up, Cloudflare's own figures for this month are fetched at most
// hourly; until they come, those fields are -1.
static R2Usage r2_usage()
{
    R2Usage u = {};
    bool ask = false;
    i64 now = now_us();
    AcquireSRWLockExclusive(&g_r2.lock);
    r2_load_locked();
    for (i32 i = 0; i < g_r2.count; i++)
        if (r2_in_bucket(g_r2.shares[i])) u.used_bytes += g_r2.shares[i].bytes;
    u.usage_us = g_r2.usage_us;
    u.bucket_bytes = u.usage_us ? g_r2.fetched.bytes : -1;
    u.class_a = u.usage_us ? g_r2.fetched.class_a : -1;
    u.class_b = u.usage_us ? g_r2.fetched.class_b : -1;
    u.account_reads = u.usage_us ? g_r2.fetched.account_reads : -1;
    u.capped = r2_capped(u.account_reads, g_app.config.r2_read_cap);
    if (r2_ready() && (!g_r2.usage_asked_us || now - g_r2.usage_asked_us > R2_USAGE_EVERY_US)) {
        g_r2.usage_asked_us = now;
        ask = true;
    }
    ReleaseSRWLockExclusive(&g_r2.lock);
    u.limit_bytes = r2_limit_for_budget(r2_budget_now());
    u.cost_per_gb_month = R2_PRICE_GB;
    u.free_gb = R2_FREE_GB;
    u.month_cost = r2_month_cost(u.bucket_bytes >= 0 ? u.bucket_bytes : u.used_bytes, u.class_a > 0 ? u.class_a : 0,
                                 u.class_b > 0 ? u.class_b : 0);
    if (ask) {
        static R2Task t;
        t = {};
        t.kind = R2_T_USAGE;
        r2_conf(&t.conf);
        SecureZeroMemory(t.conf.secret, sizeof t.conf.secret);
        t.oauth = r2_signed_in();
        if (!t.oauth) r2_token_get(t.token, sizeof t.token);
        r2_queue(t);
        SecureZeroMemory(&t, sizeof t);
    }
    return u;
}
