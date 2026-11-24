// share.cpp: Share (app.h). One click makes what the clip needs: a file at its recommended size, a file squeezed to
// the player's max size, or a link. save.cpp makes the files (share_rate) and r2.cpp the links; this file decides,
// follows the job, puts the result on the clipboard and gives the same result again for the same clip, edit and
// settings while the file is kept (share_keep_days).

static bool clipboard_copy_file(const wchar_t *path); // library.cpp
static void telemetry_shared();                        // telemetry.cpp

// The hosts a link can go to now.
struct ShareHosts {
    bool bucket;           // the player's own R2 bucket is set up
    bool catbox;           // switched on
    char uploaders[256];   // the ShareX uploaders switched on (link_uploaders), "" = none
    i64  bucket_limit;     // what the budget keeps, 0 = not checked
};

// A size as Share's menu says it: "18 MB", "2 GB" (in MiB, as Discord counts).
static void share_size_text(i64 bytes, char *out, i32 cap)
{
    f64 mb = bytes / SHARE_MIB;
    if (mb >= 1000) snprintf(out, cap, "%g GB", floor(mb / 100 + 0.5) / 10);
    else if (mb >= 10) snprintf(out, cap, "%.0f MB", mb);
    else snprintf(out, cap, "%.1f MB", mb);
}

// The lowest quality a copy may be lowered to, as a fraction of the recommended bitrate: `min_quality` percent,
// kept to 50 to 100.
static f64 share_floor(i32 min_quality)
{
    return (min_quality < 50 ? 50 : min_quality > 100 ? 100 : min_quality) / 100.0;
}

// The decision for a clip of recommended size `reco`, with quality lowered to fit down to `floor` of it. A file: as
// is under the max, squeezed to it when `floor` of the clip fits. A link: Catbox when it's on and takes the clip (as
// is up to its 200 MB, squeezed when `floor` of it fits), else the bucket as is, else the first uploader as is.
static ShareDecision share_decide_for(i64 reco, f64 max_mb, f64 floor, const ShareHosts &h)
{
    ShareDecision d = {};
    d.reco_bytes = reco;
    d.max_bytes = (i64)(max_mb * SHARE_MIB);
    d.file_squeezed = reco > d.max_bytes;
    d.file_ok = reco * floor <= d.max_bytes;
    d.file_bytes = d.file_squeezed ? (i64)(d.max_bytes * (1 - SHARE_MARGIN)) : reco;

    i64 most = 0;
    auto takes = [&](LinkHost host, bool on) {
        if (!on) return false;
        most = free_host(host)->max_bytes;
        if (reco * floor > most) return false;
        d.host = host;
        d.link_squeezed = reco > most;
        return true;
    };
    d.link_ok = true;
    if (takes(HOST_CATBOX, h.catbox)) str_set(d.host_label, "Catbox");
    else if (h.bucket) {
        d.host = HOST_R2;
        str_set(d.host_label, "your bucket");
    } else if (h.uploaders[0]) {
        d.host = HOST_CUSTOM;
        const char *end = strchr(h.uploaders, ',');
        snprintf(d.host_label, sizeof d.host_label, "%.*s", end ? (int)(end - h.uploaders) : (int)strlen(h.uploaders),
                 h.uploaders);
    } else {
        d.link_ok = false;
        str_set(d.link_why, h.catbox ? "Needs your own Cloudflare: too big for Catbox"
                                     : "Turn on Catbox or set up Cloudflare in Options");
    }
    d.link_bytes = d.link_ok && d.link_squeezed ? (i64)(most * (1 - SHARE_MARGIN)) : reco;

    char r[24], m[24];
    share_size_text(reco, r, sizeof r);
    share_size_text(d.max_bytes, m, sizeof m);
    if (!d.file_squeezed) {
        d.kind = SHARE_AS_IS;
        snprintf(d.why, sizeof d.why, "%s, as is", r);
    } else if (d.file_ok) {
        d.kind = SHARE_SQUEEZED;
        snprintf(d.why, sizeof d.why, "%s squeezed to %s", r, m);
    } else {
        d.kind = SHARE_LINK;
        if (d.link_ok) snprintf(d.why, sizeof d.why, "%s, too far over %s: a link, %s", r, m, d.host_label);
        else snprintf(d.why, sizeof d.why, "%s, too far over %s", r, m);
    }
    return d;
}

// How a clip of `reco` goes on a host that takes at most `most` (0 = no cap): as is, squeezed when `floor` of it
// fits, or not.
static ShareFit share_fit(i64 reco, f64 floor, i64 most)
{
    if (most <= 0 || reco <= most) return SHARE_FIT_AS_IS;
    return reco * floor <= most ? SHARE_FIT_SQUEEZED : SHARE_FIT_NO;
}

// The hosts on in `h` and how a clip of `reco` goes on each (share_host_choices). The bucket takes a clip as is up to
// its limit, an uploader as is.
static i32 share_hosts_for(i64 reco, f64 floor, const ShareHosts &h, ShareHostChoice *out, i32 max)
{
    i32 n = 0;
    auto add = [&](const char *name, const char *label, LinkHost host, i64 most, bool squeezes) {
        if (n >= max) return;
        ShareHostChoice &c = out[n++];
        c = {};
        str_set(c.name, name);
        str_set(c.label, label);
        c.host = host;
        c.fit = share_fit(reco, squeezes ? floor : 1, most);
        c.bytes = c.fit == SHARE_FIT_AS_IS ? reco : c.fit == SHARE_FIT_SQUEEZED ? (i64)(most * (1 - SHARE_MARGIN)) : 0;
        if (c.fit != SHARE_FIT_NO) return;
        char m[24];
        share_size_text(most, m, sizeof m);
        if (host == HOST_R2) snprintf(c.why, sizeof c.why, "Bigger than your bucket's %s: raise it in Options", m);
        else snprintf(c.why, sizeof c.why, "Too big for %s even squeezed", free_host(host)->label);
    };
    if (h.catbox) add("catbox", "Catbox", HOST_CATBOX, free_host(HOST_CATBOX)->max_bytes, true);
    if (h.bucket) add("bucket", "your bucket", HOST_R2, h.bucket_limit, false);
    for (const char *p = h.uploaders; *p;) {
        const char *end = strchr(p, ',');
        char name[64];
        snprintf(name, sizeof name, "%.*s", end ? (int)(end - p) : (int)strlen(p), p);
        if (name[0]) add(name, name, HOST_CUSTOM, 0, false);
        if (!end) break;
        p = end + 1;
    }
    return n;
}

// The decision when the player picks the link's host (a ShareHostChoice's name; "r2" is the bucket too): a link
// there as share_hosts_for says, or no link with why.
static ShareDecision share_decide_on(i64 reco, f64 max_mb, f64 floor, const ShareHosts &h, const char *host)
{
    ShareDecision d = share_decide_for(reco, max_mb, floor, h);
    d.kind = SHARE_LINK;
    d.link_ok = d.link_squeezed = false;
    d.host = HOST_R2;
    d.link_bytes = 0;
    d.host_label[0] = d.link_why[0] = 0;
    static ShareHostChoice list[24];
    i32 n = share_hosts_for(reco, floor, h, list, COUNT(list));
    const char *want = !_stricmp(host, "r2") ? "bucket" : host;
    const ShareHostChoice *c = 0;
    for (i32 i = 0; i < n && !c; i++)
        if (!_stricmp(list[i].name, want)) c = &list[i];
    char r[24];
    share_size_text(reco, r, sizeof r);
    if (!c) {
        if (!strcmp(want, "bucket")) str_set(d.link_why, "Set up your own Cloudflare in Options first");
        else snprintf(d.link_why, sizeof d.link_why, "%s isn't switched on in Options", want);
    } else if (c->fit == SHARE_FIT_NO) str_set(d.link_why, c->why);
    else {
        d.link_ok = true;
        d.host = c->host;
        d.link_squeezed = c->fit == SHARE_FIT_SQUEEZED;
        d.link_bytes = c->bytes;
        str_set(d.host_label, c->label);
    }
    if (d.link_ok) snprintf(d.why, sizeof d.why, "%s%s: a link, %s", r, d.link_squeezed ? " squeezed" : "", d.host_label);
    else snprintf(d.why, sizeof d.why, "%s: %s", r, d.link_why);
    return d;
}

// The hosts as the settings are now.
static ShareHosts share_hosts()
{
    const Config &c = g_app.config;
    ShareHosts h = {};
    h.bucket = r2_set_up();
    h.catbox = c.link_catbox;
    str_set(h.uploaders, c.link_uploaders);
    h.bucket_limit = h.bucket ? r2_limit_for_budget(r2_budget_now()) : 0;
    return h;
}

// Whether a link host that's on takes a file of `bytes` as it is: the bucket under its limit, Catbox under its own, a
// custom uploader (its limit isn't known) always.
static bool share_host_fits(i64 bytes)
{
    ShareHosts h = share_hosts();
    if (h.bucket && (!h.bucket_limit || bytes <= h.bucket_limit)) return true;
    if (h.catbox && bytes <= free_host(HOST_CATBOX)->max_bytes) return true;
    for (const char *p = h.uploaders; *p; p++)
        if (*p != ',' && *p != ' ') return true;
    return false;
}

// How long a clip plays as `edit` (0: the whole file).
static f64 share_seconds(const MediaInfo &m, const EditRecord *edit)
{
    if (!edit) return m.duration;
    return edit->mode == EDIT_ADVANCED ? adv_len(*edit) : pieces_len(*edit, m.duration);
}

static ShareDecision share_decide(const MediaInfo &m, const EditRecord *edit)
{
    const Config &c = g_app.config;
    i64 reco = share_reco_bytes(share_seconds(m, edit), m.width, m.height, m.fps, m.track_count > 0);
    f64 max_mb = c.share_max_mb > 0 ? c.share_max_mb : 20;
    return share_decide_for(reco, max_mb, share_floor(c.share_min_quality), share_hosts());
}

// Share's decision with the link's host picked (share_decide_on).
static ShareDecision share_decide_host(const MediaInfo &m, const EditRecord *edit, const char *host)
{
    const Config &c = g_app.config;
    i64 reco = share_reco_bytes(share_seconds(m, edit), m.width, m.height, m.fps, m.track_count > 0);
    f64 max_mb = c.share_max_mb > 0 ? c.share_max_mb : 20;
    return share_decide_on(reco, max_mb, share_floor(c.share_min_quality), share_hosts(), host);
}

static i32 share_host_choices(const MediaInfo &m, const EditRecord *edit, ShareHostChoice *out, i32 max)
{
    i64 reco = share_reco_bytes(share_seconds(m, edit), m.width, m.height, m.fps, m.track_count > 0);
    return share_hosts_for(reco, share_floor(g_app.config.share_min_quality), share_hosts(), out, max);
}

// ---------------------------------------------------------------------------------------------------------------
// Made files and links, for giving the same one again and for deleting with their clip:
// <local_dir>\made-shares.tsv, a line per Share, "<hash>\t<library key>\t<made, us>\t<file path or link>". Lines
// older than share_keep_days are dropped. Kept in memory once read.

struct ShareMade {
    u64  hash;
    u64  lib_key;
    i64  made_us;
    char what[MAX_PATH * 3];
};
static struct {
    ShareMade items[256]; // oldest first
    i32       count;
    bool      loaded;
    u32       gen;        // changes with the list
} g_made;

static Path share_made_path() { return path_join(g_app.paths.local_dir, L"made-shares.tsv"); }

static bool share_is_link(const char *what) { return starts_with(what, "http"); }

// What a Share makes depends on: the file (its path, size and time), the edit, file or link, and the size or host.
static u64 share_hash(const wchar_t *path, const EditRecord *edit, bool link, i64 aim, i64 limit, LinkHost host,
                      const char *uploader)
{
    u64 h = fnv64(path, wcslen(path) * sizeof(wchar_t));
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        h = fnv64(&fa.nFileSizeLow, sizeof fa.nFileSizeLow, h);
        h = fnv64(&fa.nFileSizeHigh, sizeof fa.nFileSizeHigh, h);
        h = fnv64(&fa.ftLastWriteTime, sizeof fa.ftLastWriteTime, h);
    }
    static const EditRecord none = {};
    h = fnv64(edit ? edit : &none, sizeof(EditRecord), h);
    i64 v[] = {link, aim, limit, link ? host : 0};
    h = fnv64(v, sizeof v, h);
    if (link && host == HOST_CUSTOM) h = fnv64(uploader, strlen(uploader), h);
    return h;
}

static void share_made_save()
{
    Path p = share_made_path(), tmp = p;
    wcscat_s(tmp.s, MAX_PATH, L".tmp");
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) return;
    for (i32 i = 0; i < g_made.count; i++) {
        const ShareMade &m = g_made.items[i];
        fprintf(f, "%016llx\t%016llx\t%lld\t%s\n", m.hash, m.lib_key, m.made_us, m.what);
    }
    fclose(f);
    MoveFileExW(tmp.s, p.s, MOVEFILE_REPLACE_EXISTING);
}

static void share_made_drop(i32 i)
{
    memmove(&g_made.items[i], &g_made.items[i + 1], (g_made.count - i - 1) * sizeof g_made.items[0]);
    g_made.count--;
    g_made.gen++;
}

// Reads the list once, and drops what's past share_keep_days.
static void share_made_load()
{
    if (!g_made.loaded) {
        g_made.loaded = true;
        FILE *f = _wfopen(share_made_path().s, L"rb");
        static char line[MAX_PATH * 3 + 80];
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : 0, *c = b ? strchr(b + 1, '\t') : 0;
            if (!c || !c[1]) continue;
            if (g_made.count == COUNT(g_made.items)) share_made_drop(0);
            ShareMade &m = g_made.items[g_made.count++];
            m.hash = _strtoui64(line, 0, 16);
            m.lib_key = _strtoui64(a + 1, 0, 16);
            m.made_us = _atoi64(b + 1);
            str_set(m.what, c + 1);
        }
        if (f) fclose(f);
        g_made.gen++;
    }
    i64 oldest = now_us() - (i64)(g_app.config.share_keep_days * 86400.0 * 1e6);
    bool dropped = false;
    for (i32 i = g_made.count - 1; i >= 0; i--)
        if (g_made.items[i].made_us < oldest) share_made_drop(i), dropped = true;
    if (dropped) share_made_save();
}

// Whether what was made is still there: the file kept, the link still playing.
static bool share_made_there(const char *what)
{
    if (share_is_link(what)) return r2_url_live(what);
    wchar_t p[MAX_PATH];
    utf8_to_wide(what, p, MAX_PATH);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

// The file or link made before for `hash`, still there. Its time moves up to now, so it's kept share_keep_days from
// today.
static bool share_made_find(u64 hash, char *out, i32 cap)
{
    share_made_load();
    for (i32 i = g_made.count - 1; i >= 0; i--) {
        ShareMade &m = g_made.items[i];
        if (m.hash != hash || !share_made_there(m.what)) continue;
        snprintf(out, cap, "%s", m.what);
        m.made_us = now_us();
        if (!share_is_link(m.what)) { // the share folder's cleanup goes by the file's time
            wchar_t p[MAX_PATH];
            utf8_to_wide(m.what, p, MAX_PATH);
            HANDLE fh = CreateFileW(p, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
                                    0, 0);
            if (fh != INVALID_HANDLE_VALUE) {
                FILETIME ft;
                GetSystemTimeAsFileTime(&ft);
                SetFileTime(fh, 0, 0, &ft);
                CloseHandle(fh);
            }
        }
        g_made.gen++;
        share_made_save();
        return true;
    }
    return false;
}

static void share_made_add(u64 hash, u64 lib_key, const char *what)
{
    share_made_load();
    if (g_made.count == COUNT(g_made.items)) share_made_drop(0);
    ShareMade &m = g_made.items[g_made.count++];
    m = {};
    m.hash = hash;
    m.lib_key = lib_key;
    m.made_us = now_us();
    str_set(m.what, what);
    g_made.gen++;
    share_made_save();
}

// What a Share of a link or a file makes for this clip now: the decision, the copy's aim and most (save_link's),
// the hosts for r2_share_file, and the hash it's kept under.
struct ShareAims {
    ShareDecision d;
    i64  aim, limit;
    bool squeezed;
    char hosts[64];
    u64  hash;
};
static ShareAims share_aims(bool link, const wchar_t *path, const MediaInfo &m, const EditRecord *edit,
                            const char *host = 0)
{
    ShareAims a = {};
    a.d = link && host ? share_decide_host(m, edit, host) : share_decide(m, edit);
    const ShareDecision &d = a.d;
    if (link) {
        a.aim = d.link_bytes;
        a.limit = d.host == HOST_CATBOX ? free_host(d.host)->max_bytes : 0;
        a.squeezed = d.link_squeezed;
        str_set(a.hosts, d.host == HOST_R2 ? "r2" : d.host == HOST_CATBOX ? "catbox" : d.host_label);
    } else {
        a.aim = d.file_squeezed ? d.max_bytes : d.reco_bytes;
        a.limit = d.max_bytes;
        a.squeezed = d.file_squeezed;
    }
    a.hash = share_hash(path, edit, link, a.aim, a.limit, d.host, d.host_label);
    return a;
}

// For r2.cpp, a link started before the clip was probed (the pipe's "link", sharing a link again): where it goes,
// on `host` when one is picked (0: Share's rule).
static bool share_link_plan(const wchar_t *path, const MediaInfo &m, const EditRecord *edit, const char *host,
                            char *hosts, i32 cap, i64 *aim, i64 *limit, char *why, i32 why_cap)
{
    ShareAims a = share_aims(true, path, m, edit, host);
    if (!a.d.link_ok) {
        snprintf(why, why_cap, "%s", a.d.link_why);
        return false;
    }
    snprintf(hosts, cap, "%s", a.hosts);
    *aim = a.aim;
    *limit = a.limit;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Jobs (main thread)

struct ShareJob {
    ShareJobView v;
    u32  save;      // the save making a file
    u32  r2;        // the r2 job making a link
    u64  hash, lib_key;
    bool squeezed;  // the file or link copy is squeezed
    bool cancelled; // share_cancel ended it; `save` is followed until it stops, and its file deleted
    char name[200];
};
static struct {
    ShareJob jobs[16];
    u32      next_id;
} g_share;

static bool share_holds_save(u32 id)
{
    for (const ShareJob &j : g_share.jobs)
        if (id && j.save == id && (j.v.state < SHARE_DONE || j.cancelled)) return true;
    return false;
}

// A slot for a new job: a free one, else the oldest finished.
static ShareJob *share_new_job()
{
    ShareJob *pick = 0;
    for (ShareJob &j : g_share.jobs) {
        if (!j.v.id) return &j;
        if (j.v.state >= SHARE_DONE && (!pick || j.v.id < pick->v.id)) pick = &j;
    }
    return pick;
}

// Ends a job: the result goes on the clipboard and is kept for next time.
static void share_end(ShareJob &j, bool ok, const char *what, const char *error)
{
    j.v.state = ok ? SHARE_DONE : SHARE_FAILED;
    j.v.verb[0] = 0;
    j.v.progress = ok ? 1.0f : 0;
    if (!ok) {
        str_set(j.v.error, error);
        log_line("share %u: failed: %s", j.v.id, error);
        return;
    }
    if (j.v.kind == SHARE_LINK) {
        str_set(j.v.url, what);
        ImGui::SetClipboardText(what);
    } else {
        str_set(j.v.path, what);
        wchar_t w[MAX_PATH];
        utf8_to_wide(what, w, MAX_PATH);
        if (!clipboard_copy_file(w)) str_set(j.v.error, "The clipboard was busy: copy it again");
    }
    if (!j.v.reused) share_made_add(j.hash, j.lib_key, what);
    else if (j.v.kind != SHARE_LINK) telemetry_shared(); // a made file is counted as its save ends
    log_line("share %u: %s %s", j.v.id, j.v.reused ? "gave again" : "made", what);
}

// Follows the saves and links under way. On EV_SAVE and EV_R2, and whenever a job is looked at.
static void share_poll()
{
    for (ShareJob &j : g_share.jobs) {
        if (j.cancelled && j.save) { // a cancelled copy that finished anyway goes
            const SaveJobView *s = save_view(j.save);
            if (s && s->state == SAVE_DONE) {
                wchar_t w[MAX_PATH];
                utf8_to_wide(s->path, w, MAX_PATH);
                DeleteFileW(w);
                log_line("share %u: the copy finished as it was cancelled: deleted", j.v.id);
            }
            if (!s || s->state >= SAVE_DONE) j.save = 0;
        }
        if (!j.v.id || j.v.state >= SHARE_DONE) continue;
        if (j.save) {
            const SaveJobView *s = save_view(j.save);
            if (!s) share_end(j, false, "", "The copy was lost");
            else if (s->state == SAVE_DONE) share_end(j, true, s->path, "");
            else if (s->state == SAVE_FAILED) share_end(j, false, "", s->error);
            else if (s->state == SAVE_CANCELLED) share_end(j, false, "", "Cancelled");
            else j.v.progress = s->progress;
        } else if (j.r2) {
            R2Job r = r2_job(j.r2);
            if (!r.id) share_end(j, false, "", "The link was lost");
            else if (r.state == R2_DONE) share_end(j, true, r.url, "");
            else if (r.state == R2_FAILED) share_end(j, false, "", r.error);
            else {
                bool up = r.state == R2_UPLOADING;
                j.v.state = up ? SHARE_UPLOADING : SHARE_MAKING;
                str_set(j.v.verb, up ? "Uploading" : j.squeezed ? "Squeezing" : "Making");
                j.v.progress = r.progress;
            }
        }
    }
}

static u32 share_start(ShareKind kind, const wchar_t *path, const MediaInfo &m, const EditRecord *edit,
                       const char *name, const char *game, u64 lib_key, const char *host)
{
    if (host && !host[0]) host = 0;
    if (host) kind = SHARE_LINK;
    ShareJob *jp = share_new_job();
    if (!jp) return 0;
    ShareJob &j = *jp;
    j = {};
    j.v.id = ++g_share.next_id;
    j.v.kind = kind == SHARE_LINK ? SHARE_LINK : SHARE_AS_IS;
    j.v.state = SHARE_MAKING;
    str_set(j.name, name && name[0] ? name : "clip");
    ShareDecision d = host ? share_decide_host(m, edit, host) : share_decide(m, edit);
    bool link = kind == SHARE_LINK;
    if (link && !d.link_ok) {
        share_end(j, false, "", d.link_why);
        return j.v.id;
    }
    if (!link && !d.file_ok) {
        char mb[24];
        share_size_text(d.max_bytes, mb, sizeof mb);
        char why[160];
        snprintf(why, sizeof why, "Too big for %s even squeezed: share a link", mb);
        share_end(j, false, "", why);
        return j.v.id;
    }
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        share_end(j, false, "", "The file isn't there any more");
        return j.v.id;
    }
    ShareAims a = share_aims(link, path, m, edit, host);
    j.squeezed = a.squeezed;
    if (!link && j.squeezed) j.v.kind = SHARE_SQUEEZED;
    j.hash = a.hash;
    j.lib_key = lib_key;
    char made[MAX_PATH * 3];
    if (share_made_find(j.hash, made, sizeof made)) {
        j.v.reused = true;
        share_end(j, true, made, "");
        return j.v.id;
    }
    str_set(j.v.verb, j.squeezed ? "Squeezing" : "Making");
    if (link) {
        j.r2 = r2_share_file(path, j.name, game, lib_key, &m, edit, a.hosts, a.aim, a.limit);
        if (!j.r2) {
            share_end(j, false, "", "Too many links are under way: try again in a moment");
            return j.v.id;
        }
    } else {
        static EditJob e;
        e = {};
        e.src.s[0] = 0;
        wcsncpy(e.src.s, path, MAX_PATH - 1);
        e.info = m;
        if (edit) e.edit = *edit;
        e.out_dir = g_app.paths.share_dir;
        str_set(e.stem, j.name);
        clean_name(e.stem);
        if (!e.stem[0]) str_set(e.stem, "clip");
        e.aim_bytes = a.aim;
        e.limit_bytes = a.limit;
        j.save = save_edit(SAVE_SHARE, e);
        if (!j.save) {
            share_end(j, false, "", "Too many saves are waiting: try again in a moment");
            return j.v.id;
        }
    }
    log_line("share %u: %s of %s, %s (recommended %.1f MB, max %.1f MB)", j.v.id, link ? "link" : "file", j.name,
             link ? d.host_label : j.squeezed ? "squeezed" : "as is", d.reco_bytes / SHARE_MIB,
             d.max_bytes / SHARE_MIB);
    share_poll();
    return j.v.id;
}

static ShareJobView share_job(u32 id)
{
    share_poll();
    for (const ShareJob &j : g_share.jobs)
        if (id && j.v.id == id) return j.v;
    return {};
}

// Stops a Share at once and deletes what it made: a file being made (or made just now, not yet given out), and a
// link's copy and upload (r2_cancel). Nothing goes on the clipboard.
static void share_cancel(u32 id)
{
    for (ShareJob &j : g_share.jobs) {
        if (!id || j.v.id != id || j.v.state >= SHARE_DONE) continue;
        if (j.save) save_cancel(j.save);
        if (j.r2) r2_cancel(j.r2);
        j.cancelled = true;
        share_end(j, false, "", "Cancelled");
    }
    share_poll();
}

// ---------------------------------------------------------------------------------------------------------------
// Per clip (by library key)

// The clip's newest link that hasn't been removed (it may be paused). Its whole history entry, for the
// menu's link row: r2_pause, r2_resume and r2_delete take its key.
static bool share_clip_link(u64 lib_key, R2Share *out)
{
    static R2Share list[R2_MAX_SHARES];
    static i32 count = -1;
    static u32 gen;
    if (count < 0 || gen != r2_history_gen()) {
        gen = r2_history_gen();
        count = r2_history(list, COUNT(list)); // newest first
    }
    for (i32 i = 0; i < count && lib_key; i++) {
        const R2Share &s = list[i];
        if (s.lib_key != lib_key || s.removed) continue;
        if (out) *out = s;
        return true;
    }
    return false;
}

// What the clip has now: a share file made for its edit and the settings as they are, and a link. Cheap: looked up
// again when the edit, the made list or the links change, else once a second.
static ClipShareState share_clip_state(u64 lib_key, const wchar_t *path, const MediaInfo &m, const EditRecord *edit)
{
    struct Seen {
        u64 lib_key, edit_hash;
        u32 made_gen, r2_gen;
        i64 at_us;
        ClipShareState s;
    };
    static Seen seen[64];
    static i32 next;
    share_made_load();
    u64 eh = edit ? fnv64(edit, sizeof *edit) : 0;
    u32 rg = r2_history_gen();
    i64 now = now_us();
    for (const Seen &s : seen)
        if (lib_key && s.lib_key == lib_key && s.edit_hash == eh && s.made_gen == g_made.gen && s.r2_gen == rg &&
            now - s.at_us < 1000000)
            return s.s;
    ClipShareState st = {};
    u64 hash = share_aims(false, path, m, edit).hash;
    for (i32 i = g_made.count - 1; i >= 0 && !st.file; i--)
        if (g_made.items[i].hash == hash && share_made_there(g_made.items[i].what)) st.file = true;
    R2Share l;
    if (share_clip_link(lib_key, &l)) {
        st.link = true;
        st.link_paused = l.paused;
        st.host = l.host;
    }
    Seen *slot = 0;
    for (Seen &s : seen)
        if (s.lib_key == lib_key) slot = &s;
    if (!slot) slot = &seen[next++ % COUNT(seen)];
    *slot = {lib_key, eh, g_made.gen, rg, now, st};
    return st;
}

// What deleting the clip takes with it: its share files, and its links where they can be deleted (the bucket's).
// Catbox and uploader links can't be: `cant` says how many stay up ("" for none). Returns how many files and links go.
static i32 share_clip_delete_preview(u64 lib_key, char *cant, i32 cap)
{
    if (cap > 0) cant[0] = 0;
    if (!lib_key) return 0;
    share_made_load();
    i32 n = 0;
    for (i32 i = 0; i < g_made.count; i++)
        if (g_made.items[i].lib_key == lib_key && !share_is_link(g_made.items[i].what) &&
            share_made_there(g_made.items[i].what))
            n++;
    static R2Share list[R2_MAX_SHARES];
    i32 count = r2_history(list, COUNT(list)), stay = 0;
    const char *host = 0;
    for (i32 i = 0; i < count; i++) {
        const R2Share &s = list[i];
        if (s.lib_key != lib_key || s.removed) continue;
        if (s.host == HOST_R2) n++;
        else if (s.host == HOST_CATBOX || s.host == HOST_CUSTOM) {
            const char *h = s.host == HOST_CATBOX ? "Catbox" : "uploader";
            host = !host || !strcmp(host, h) ? h : "";
            stay++;
        }
    }
    if (stay && cap > 0)
        snprintf(cant, cap, "%d %s%slink%s can't be removed and stay%s up", stay, host, host[0] ? " " : "",
                 stay > 1 ? "s" : "", stay > 1 ? "" : "s");
    return n;
}

// Deletes the clip's share files and links (share_clip_delete_preview). The clip itself is the caller's.
static void share_clip_delete(u64 lib_key)
{
    if (!lib_key) return;
    share_made_load();
    bool changed = false;
    for (i32 i = g_made.count - 1; i >= 0; i--) {
        if (g_made.items[i].lib_key != lib_key) continue;
        if (!share_is_link(g_made.items[i].what)) {
            wchar_t p[MAX_PATH];
            utf8_to_wide(g_made.items[i].what, p, MAX_PATH);
            DeleteFileW(p);
        }
        share_made_drop(i);
        changed = true;
    }
    if (changed) share_made_save();
    static R2Share list[R2_MAX_SHARES];
    i32 count = r2_history(list, COUNT(list)), links = 0;
    for (i32 i = 0; i < count; i++)
        if (list[i].lib_key == lib_key) r2_delete(list[i].key), links++;
    log_line("share: deleted the share files and %d links of a clip", links);
}
