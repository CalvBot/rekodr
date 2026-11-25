// Audio: a WASAPI capture stream for each app playing sound (process loopback of its whole process tree), plus
// Windows' default input and any other inputs turned on in Options, each piped as raw PCM into its own small ffmpeg that writes Opus in 10 s segments to the buffer. Also which
// game is being played (activity.tsv).
//
// Threads:
// - The watcher (one): every second it lists processes and the PIDs with an active audio session, starts a stream
//   for each app that isn't ignored and closes streams whose app went quiet for 30 s or exited. Every 2 s it works
//   out the game being played. It owns g_aw's workers and its own copy of the settings.
// - One worker per stream: WASAPI -> ffmpeg. It owns its client and ffmpeg pipe; it only shares its stop and
//   finished flags with the watcher.
// Neither touches g_app: they post EV_AUDIO events, applied by audio_on_event on the main thread.
//
// On disk:
//   <buffer>\audio\<yyyymmdd_hhmmss_mmm>_<exe>_<pid>\a_00000.mka ...  Opus in Matroska, 10 s segments
//                                                    list.csv        written by ffmpeg's segment muxer
//                                                    meta.json       {"kind","exe","pid","start_us"}
//                                                    peaks.bin       the loudest of each 100 ms, a byte each
//                                                    ffmpeg.log      ffmpeg's errors
//   <buffer>\activity.tsv   "<us>\t<exe>" each time the game being played changes (empty exe = the desktop)
//   <buffer>\focus.tsv      "<us>\t<exe>" each time the program in front changes, whatever it is (empty = none)
// start_us is the wall-clock time of the first sample, from WASAPI's QPC packet times; gaps are padded with silence
// and late audio is trimmed, so sample n of a stream is at start_us + n / 48000 s and lines up with the video.
// peaks.bin is the timeline's waveform, measured as the audio is written: byte n is the loudest sample from
// start_us + n * 100 ms, as the square root of its level times 255. It's written a second at a time.

#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <audioclientactivationparams.h>
#include <tlhelp32.h>

#define AUDIO_RATE        48000
#define AUDIO_FRAME_BYTES 8 // stereo f32

static const i64 AUDIO_LINGER_US = 30000000; // a stream closes once its app has been quiet this long
static const i64 AUDIO_GAP_US    = 20000;    // pad gaps longer than this with silence...
static const i64 AUDIO_LATE_US   = 40000;    // ...and trim audio arriving later than this, keeping the stream on time

enum AudioEventKind : u8 {
    AE_NONE,
    AE_STARTED, // b = stream id | kind << 32, text = "exe	path"
    AE_STOPPED, // b = stream id
    AE_HEARD,   // b = us, text = "exe	path": an app played sound (added to Config.apps as new if it isn't known)
    AE_GAME,    // text = the game being played, empty on the desktop
    AE_WINDOW,  // b = the game window to record (an HWND), 0 = the monitor
};

// ---------------------------------------------------------------------------------------------------------------
// Helpers that only read their inputs (safe on any thread)

// An app's category: its own, else its folder rule's by `path` (where the process runs from), else new. Without a
// path, where the app was last heard from.
static AppCategory audio_category_at(const Config &c, const char *exe, const char *path)
{
    return app_category_in(c.apps, c.app_count, c.folders, c.folder_count, exe, path);
}

// Splits an event's "exe	path".
static void split_exe_path(const char *text, Str64 *exe, char *path, i32 cap)
{
    const char *tab = strchr(text, '	');
    i32 n = tab ? (i32)(tab - text) : (i32)strlen(text);
    if (n >= (i32)sizeof exe->s) n = (i32)sizeof exe->s - 1;
    memcpy(exe->s, text, n);
    exe->s[n] = 0;
    path[0] = 0;
    if (tab) snprintf(path, cap, "%s", tab + 1);
}

// A process's exe path in UTF-8; "" when it can't be opened (elevated, or gone).
static void proc_image_path(u32 pid, char *out, i32 cap)
{
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    wchar_t w[MAX_PATH];
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, w, &len)) wide_to_utf8(w, out, cap);
    CloseHandle(h);
}

// Exe paths of processes that played sound, so each is opened once rather than every tick.
struct SeenProc {
    u32   pid;
    Str64 exe;
    i64   heard_us; // last AE_HEARD posted for it while ignored
    char  path[MAX_PATH];
};
static SeenProc g_seen[32];
static i32 g_seen_next;

static SeenProc *seen_proc(u32 pid, const char *exe)
{
    for (i32 i = 0; i < COUNT(g_seen); i++)
        if (g_seen[i].pid == pid && str_eq(g_seen[i].exe.s, exe)) return &g_seen[i];
    SeenProc *s = &g_seen[g_seen_next];
    g_seen_next = (g_seen_next + 1) % COUNT(g_seen);
    *s = {};
    s->pid = pid;
    str_set(s->exe.s, exe);
    proc_image_path(pid, s->path, sizeof s->path);
    return s;
}

static AudioTrack audio_track(const Config &c, AudioKind kind, const char *exe, const char *path)
{
    if (kind == AUDIO_MIC) return TRACK_MIC;
    switch (audio_category_at(c, exe, path)) {
    case CAT_COMMUNICATION: return TRACK_COMMUNICATION;
    case CAT_GAME: return TRACK_GAME;
    default: return TRACK_APP;
    }
}

// "Overwatch" for overwatch.exe, unless the app has a name in the settings.
static void audio_display_name(const Config &c, const char *exe, char *out, i32 cap)
{
    for (i32 i = 0; i < c.app_count; i++) {
        // A name given in Options, else the exe's File description, else the exe name
        const AppInfo &a = c.apps[i];
        if (strcmp(a.exe.s, exe)) continue;
        if (a.name.s[0] || a.desc.s[0]) {
            snprintf(out, cap, "%s", a.name.s[0] ? a.name.s : a.desc.s);
            return;
        }
    }
    snprintf(out, cap, "%s", exe);
    size_t n = strlen(out);
    if (n > 4 && !strcmp(out + n - 4, ".exe")) out[n - 4] = 0;
    out[0] = (char)toupper((u8)out[0]);
}

// The performance counter in 100 ns units: the clock WASAPI stamps packets with.
static i64 qpc_100ns()
{
    LARGE_INTEGER count, freq;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&freq);
    if (!freq.QuadPart) return 0;
    i64 secs = count.QuadPart / freq.QuadPart, rest = count.QuadPart % freq.QuadPart;
    return secs * 10000000 + rest * 10000000 / freq.QuadPart; // split so it can't overflow
}

static i64 qpc_to_wall_us(u64 qpc)
{
    i64 now = now_us(), q = qpc_100ns();
    return now - (q - (i64)qpc) / 10;
}

// The stream folder's name part for an exe: "discord" from discord.exe, anything odd as '_', at most 40 chars.
static void audio_file_safe(const char *exe, wchar_t *out, i32 cap)
{
    wchar_t w[MAX_PATH];
    utf8_to_wide(exe, w, COUNT(w));
    size_t n = wcslen(w);
    while (n >= 4 && !wcscmp(w + n - 4, L".exe")) w[n -= 4] = 0;
    i32 o = 0;
    for (size_t i = 0; i < n && o < 40 && o + 1 < cap; i++) {
        wchar_t c = w[i];
        out[o++] = (iswalnum(c) || c == L'-' || c == L'_') ? c : L'_';
    }
    out[o] = 0;
}

static bool write_whole_file(const wchar_t *path, const char *text)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, text, (DWORD)strlen(text), &written, 0);
    CloseHandle(f);
    return ok && written == strlen(text);
}

// ---------------------------------------------------------------------------------------------------------------
// Writer: one stream's PCM into its ffmpeg, kept on the wall clock

enum { AUDIO_PEAK_FRAMES = AUDIO_RATE / 10 }; // a peaks.bin byte's worth of audio

struct AudioWriter {
    bool   open;
    Proc   proc;
    HANDLE log;      // ffmpeg.log
    HANDLE peaks;    // peaks.bin
    i64    start_us; // wall clock of sample 0
    i64    frames;   // written so far
    f32    peak;     // the loudest so far of the 100 ms being measured
    u8     pending[10];
    i32    pending_count;
};

// Measures what's written into peaks.bin
static void writer_peaks(AudioWriter *w, const u8 *bytes, u32 n)
{
    if (w->peaks == INVALID_HANDLE_VALUE) return;
    const f32 *s = (const f32 *)bytes;
    i64 frames = n / AUDIO_FRAME_BYTES, at = w->frames;
    for (i64 i = 0; i < frames; i++) {
        f32 l = fabsf(s[2 * i]), r = fabsf(s[2 * i + 1]), m = l > r ? l : r;
        if (m > w->peak) w->peak = m;
        if ((at + i + 1) % AUDIO_PEAK_FRAMES) continue;
        f32 v = w->peak > 1 ? 1 : w->peak;
        w->pending[w->pending_count++] = (u8)(sqrtf(v) * 255 + 0.5f);
        w->peak = 0;
        if (w->pending_count == COUNT(w->pending)) {
            DWORD written;
            WriteFile(w->peaks, w->pending, w->pending_count, &written, 0);
            w->pending_count = 0;
        }
    }
}

static bool pipe_write_all(HANDLE pipe, const u8 *data, u32 n)
{
    while (n) {
        DWORD written = 0;
        if (!WriteFile(pipe, data, n, &written, 0) || !written) return false;
        data += written;
        n -= written;
    }
    return true;
}

// Copies whatever ffmpeg wrote to stderr into ffmpeg.log, without blocking.
static void writer_drain_log(AudioWriter *w)
{
    if (!w->proc.err) return;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(w->proc.err, 0, 0, 0, &avail, 0) || !avail) return;
        char buf[1024];
        DWORD got = 0;
        if (!ReadFile(w->proc.err, buf, avail < sizeof buf ? avail : sizeof buf, &got, 0) || !got) return;
        DWORD written;
        if (w->log != INVALID_HANDLE_VALUE) WriteFile(w->log, buf, got, &written, 0);
    }
}

static i64 writer_expected_us(const AudioWriter *w)
{
    return w->start_us + w->frames * 1000000 / AUDIO_RATE;
}

static bool writer_write(AudioWriter *w, const u8 *bytes, u32 n)
{
    if (!pipe_write_all(w->proc.in, bytes, n)) return false;
    writer_peaks(w, bytes, n);
    w->frames += n / AUDIO_FRAME_BYTES;
    return true;
}

static bool writer_silence(AudioWriter *w, i64 frames)
{
    static const u8 zeros[8192 * AUDIO_FRAME_BYTES] = {};
    while (frames > 0) {
        i64 chunk = frames < 8192 ? frames : 8192;
        if (!writer_write(w, zeros, (u32)(chunk * AUDIO_FRAME_BYTES))) return false;
        frames -= chunk;
    }
    return true;
}

// Adds a packet that started at `t_us`: pads the gap before it, or trims it if it overlaps what's written.
static bool writer_place(AudioWriter *w, const u8 *bytes, u32 n, i64 t_us)
{
    i64 diff = t_us - writer_expected_us(w);
    if (diff > AUDIO_GAP_US) {
        if (!writer_silence(w, diff * AUDIO_RATE / 1000000)) return false;
    } else if (diff < -AUDIO_LATE_US) {
        i64 drop = (-diff * AUDIO_RATE / 1000000) * AUDIO_FRAME_BYTES;
        if (drop > n) drop = n;
        bytes += drop;
        n -= (u32)drop;
    }
    return writer_write(w, bytes, n);
}

// Nothing arrived for a while (the app is quiet): fill with silence up to shortly before now.
static bool writer_idle(AudioWriter *w)
{
    i64 behind = now_us() - 150000 - writer_expected_us(w);
    if (behind > 300000) return writer_silence(w, behind * AUDIO_RATE / 1000000);
    return true;
}

static bool writer_open(AudioWriter *w, AudioKind kind, const char *exe, const char *exe_path, u32 pid, const Path &buffer_dir,
                        const Path &ffmpeg, i64 start_us)
{
    *w = {};
    w->log = w->peaks = INVALID_HANDLE_VALUE;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t safe[48];
    audio_file_safe(exe, safe, COUNT(safe));
    Path dir = {};
    swprintf(dir.s, MAX_PATH, L"%s\\audio\\%04d%02d%02d_%02d%02d%02d_%03d_%s_%u", buffer_dir.s, t.wYear, t.wMonth,
             t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, safe, pid);
    int made = SHCreateDirectoryExW(0, dir.s, 0);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS) {
        log_line("%s: couldn't create the audio folder %ls (%d)", exe, dir.s, made);
        return false;
    }

    auto escape = [](const char *in, char *out, i32 cap) {
        i32 e = 0;
        for (const char *c = in; *c && e < cap - 8; c++) {
            if (*c == '"' || *c == '\\') { out[e++] = '\\'; out[e++] = *c; }
            else if ((u8)*c < 0x20) e += snprintf(out + e, cap - e, "\\u%04x", (u8)*c);
            else out[e++] = *c;
        }
        out[e] = 0;
    };
    char escaped[256], escaped_path[MAX_PATH * 2];
    escape(exe, escaped, sizeof escaped);
    escape(exe_path, escaped_path, sizeof escaped_path);
    char meta[1024];
    snprintf(meta, sizeof meta, "{\"kind\":\"%s\",\"exe\":\"%s\",\"path\":\"%s\",\"pid\":%u,\"start_us\":%lld}",
             kind == AUDIO_MIC ? "mic" : "app", escaped, escaped_path, pid, (long long)start_us);
    if (!write_whole_file(path_join(dir, L"meta.json").s, meta)) {
        log_line("%s: couldn't write meta.json (%d)", exe, (int)GetLastError());
        return false;
    }

    static const char *fixed[] = {
        "-hide_banner", "-loglevel", "error",
        "-f", "f32le", "-ar", "48000", "-ac", "2", "-i", "pipe:0",
        "-c:a", "libopus", "-b:a", "128k", "-application", "audio",
        "-f", "segment", "-segment_time", "10", "-segment_format", "matroska",
        // Small clusters, flushed, so a clip can read the segment still being written. flush_packets has to be
        // given to the inner Matroska muxer too; on the segment muxer alone the open file stays empty.
        "-segment_format_options", "cluster_time_limit=1000:flush_packets=1", "-flush_packets", "1",
        "-reset_timestamps", "0", "-segment_list_type", "csv", "-segment_list",
    };
    wchar_t args[4096] = {};
    for (const char *a : fixed) cmd_arg8(args, COUNT(args), a);
    cmd_arg(args, COUNT(args), path_join(dir, L"list.csv").s);
    cmd_arg(args, COUNT(args), path_join(dir, L"a_%05d.mka").s);

    w->log = CreateFileW(path_join(dir, L"ffmpeg.log").s, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, 0);
    if (!proc_start(ffmpeg.s, args, PROC_STDIN | PROC_STDERR, &w->proc)) {
        if (w->log != INVALID_HANDLE_VALUE) CloseHandle(w->log);
        *w = {};
        w->log = w->peaks = INVALID_HANDLE_VALUE;
        return false;
    }
    // Shared for writing too: Delete footage zeroes stretches of it (erase.cpp)
    w->peaks = CreateFileW(path_join(dir, L"peaks.bin").s, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    w->open = true;
    w->start_us = start_us;
    char name[MAX_PATH * 3];
    wide_to_utf8(wcsrchr(dir.s, L'\\') + 1, name, sizeof name);
    log_line("audio stream %s started", name);
    return true;
}

// Ends the input so ffmpeg finishes the last segment, then waits for it (up to 10 s).
static void writer_finish(AudioWriter *w, const char *exe)
{
    if (!w->open) return;
    proc_close_stdin(&w->proc);
    i32 code = -1;
    for (i32 i = 0; i < 100 && code < 0; i++) {
        writer_drain_log(w);
        code = proc_wait(&w->proc, 100);
    }
    writer_drain_log(w);
    if (code < 0) {
        log_line("%s: the audio encoder didn't finish; stopping it", exe);
        proc_kill(&w->proc);
    } else if (code != 0) {
        log_line("%s: the audio encoder exited with %d (see its ffmpeg.log)", exe, code);
    }
    proc_close(&w->proc);
    if (w->log != INVALID_HANDLE_VALUE) CloseHandle(w->log);
    if (w->peaks != INVALID_HANDLE_VALUE) {
        DWORD written;
        if (w->pending_count) WriteFile(w->peaks, w->pending, w->pending_count, &written, 0);
        CloseHandle(w->peaks);
    }
    *w = {};
    w->log = w->peaks = INVALID_HANDLE_VALUE;
}

// ---------------------------------------------------------------------------------------------------------------
// WASAPI

// ActivateAudioInterfaceAsync wants a completion handler object. It only signals an event; the caller waits on it.
// A plain struct laid out like a COM object (vtable pointer first), on the heap because the system may release it
// after the caller has moved on.
struct ActivateHandler;
struct ActivateHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ActivateHandler *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ActivateHandler *);
    ULONG(STDMETHODCALLTYPE *Release)(ActivateHandler *);
    HRESULT(STDMETHODCALLTYPE *ActivateCompleted)(ActivateHandler *, IActivateAudioInterfaceAsyncOperation *);
};
struct ActivateHandler {
    const ActivateHandlerVtbl *vtbl;
    LONG   refs;
    HANDLE done;
};

static ULONG STDMETHODCALLTYPE ah_add_ref(ActivateHandler *h) { return (ULONG)InterlockedIncrement(&h->refs); }

static ULONG STDMETHODCALLTYPE ah_release(ActivateHandler *h)
{
    LONG left = InterlockedDecrement(&h->refs);
    if (left == 0) {
        CloseHandle(h->done);
        HeapFree(GetProcessHeap(), 0, h);
    }
    return (ULONG)left;
}

static HRESULT STDMETHODCALLTYPE ah_query_interface(ActivateHandler *h, REFIID iid, void **out)
{
    // Agile, so the system can call it from any thread.
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
        iid == __uuidof(IAgileObject)) {
        ah_add_ref(h);
        *out = h;
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE ah_completed(ActivateHandler *h, IActivateAudioInterfaceAsyncOperation *)
{
    SetEvent(h->done);
    return S_OK;
}

static const ActivateHandlerVtbl g_activate_handler_vtbl = {ah_query_interface, ah_add_ref, ah_release, ah_completed};

// An audio client capturing `pid` and its child processes.
static HRESULT activate_process_loopback(u32 pid, IAudioClient **out)
{
    *out = 0;
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv = {};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof params;
    pv.blob.pBlobData = (BYTE *)&params;

    ActivateHandler *h = (ActivateHandler *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *h);
    CHECK(h, "out of memory for an activation handler");
    h->vtbl = &g_activate_handler_vtbl;
    h->refs = 1;
    h->done = CreateEventW(0, TRUE, FALSE, 0);
    IActivateAudioInterfaceAsyncOperation *op = 0;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
                                             (IActivateAudioInterfaceCompletionHandler *)h, &op);
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObject(h->done, 5000) != WAIT_OBJECT_0) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        } else {
            HRESULT activated = E_FAIL;
            IUnknown *unk = 0;
            hr = op->GetActivateResult(&activated, &unk);
            if (SUCCEEDED(hr)) hr = activated;
            if (SUCCEEDED(hr) && unk) hr = unk->QueryInterface(__uuidof(IAudioClient), (void **)out);
            if (unk) unk->Release();
        }
    }
    if (op) op->Release();
    ah_release(h);
    return hr;
}

static IMMDeviceEnumerator *device_enumerator()
{
    IMMDeviceEnumerator *en = 0;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&en);
    return en;
}

// The default mic's endpoint id, empty if there's none.
static void default_mic_id(wchar_t *out, i32 cap)
{
    out[0] = 0;
    IMMDeviceEnumerator *en = device_enumerator();
    if (!en) return;
    IMMDevice *dev = 0;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev))) {
        wchar_t *id = 0;
        if (SUCCEEDED(dev->GetId(&id))) {
            wcsncpy_s(out, cap, id, _TRUNCATE);
            CoTaskMemFree(id);
        }
        dev->Release();
    }
    en->Release();
}

static void device_name(IMMDevice *dev, char *out, i32 cap)
{
    // PKEY_Device_FriendlyName, spelled out so no header has to define the key's storage.
    static const PROPERTYKEY friendly = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    snprintf(out, cap, "default device");
    IPropertyStore *props = 0;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(props->GetValue(friendly, &v)) && v.vt == VT_LPWSTR) wide_to_utf8(v.pwszVal, out, cap);
    PropVariantClear(&v);
    props->Release();
}

static void device_id_utf8(IMMDevice *dev, char *out, i32 cap)
{
    out[0] = 0;
    wchar_t *id = 0;
    if (SUCCEEDED(dev->GetId(&id))) {
        wide_to_utf8(id, out, cap);
        CoTaskMemFree(id);
    }
}

// The inputs plugged in now, and which is Windows' default. For Options (main thread) and the watcher.
static i32 audio_list_inputs(InputDevice *out, i32 cap, char *default_id, i32 default_cap)
{
    i32 n = 0;
    default_id[0] = 0;
    IMMDeviceEnumerator *en = device_enumerator();
    if (!en) return 0;
    IMMDevice *def = 0;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &def))) {
        device_id_utf8(def, default_id, default_cap);
        def->Release();
    }
    IMMDeviceCollection *devices = 0;
    UINT count = 0;
    if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) devices->GetCount(&count);
    for (UINT i = 0; i < count && n < cap; i++) {
        IMMDevice *dev = 0;
        if (FAILED(devices->Item(i, &dev))) continue;
        InputDevice *d = &out[n++];
        *d = {};
        device_id_utf8(dev, d->id, sizeof d->id);
        device_name(dev, d->name, sizeof d->name);
        dev->Release();
    }
    if (devices) devices->Release();
    en->Release();
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Workers: one thread per stream

struct AudioWorker {
    // Set by the watcher before the thread starts, read-only after.
    bool      used;
    u32       id;
    AudioKind kind;
    Str64     exe;
    char      path[MAX_PATH]; // its exe's full path (UTF-8): folder rules decide by it
    u32       pid; // the process tree's root (0 for an input)
    Path      buffer_dir;
    Path      ffmpeg;
    // Shared between the watcher and the worker.
    volatile LONG stop;
    volatile LONG finished;
    HANDLE        thread;
    // The watcher's own.
    i64 last_playing_us;
    i64 last_heard_us; // last AE_HEARD posted
};

enum CaptureEnd { CAPTURE_STOP, CAPTURE_RESTART, CAPTURE_ERROR };

#define AUDIO_PACKET_CAP (AUDIO_RATE * AUDIO_FRAME_BYTES) // one second, far more than one packet ever holds

// Captures until told to stop, the source fails, or (mic) the default device changes. Each call is one stream
// folder, opened on the first packet so start_us is the first sample's time.
static CaptureEnd audio_capture(AudioWorker *w, u8 *packet)
{
    IMMDeviceEnumerator *en = 0;
    IMMDevice *dev = 0;
    IAudioClient *client = 0;
    IAudioCaptureClient *capture = 0;
    HANDLE ready = 0;
    AudioWriter writer = {};
    wchar_t mic_id[256] = {};
    CaptureEnd end = CAPTURE_ERROR;
    const char *what = "";
    HRESULT hr = S_OK;
    u64 mic_checked = GetTickCount64();
    bool warned_big = false;
    WAVEFORMATEX fmt = {};
    DWORD flags = 0;

    if (w->kind == AUDIO_MIC) {
        what = "opening the mic";
        en = device_enumerator();
        if (!en) { hr = E_FAIL; goto done; }
        if (w->path[0]) { // another input, by its endpoint id
            wchar_t want[256];
            utf8_to_wide(w->path, want, COUNT(want));
            hr = en->GetDevice(want, &dev);
        } else {
            hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
        }
        if (FAILED(hr)) goto done;
        wchar_t *id = 0;
        if (SUCCEEDED(dev->GetId(&id))) {
            wcsncpy_s(mic_id, id, _TRUNCATE);
            CoTaskMemFree(id);
        }
        char name[256];
        device_name(dev, name, sizeof name);
        log_line(w->path[0] ? "input: %s" : "mic: %s", name);
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void **)&client);
    } else {
        what = "starting process loopback";
        hr = activate_process_loopback(w->pid, &client);
    }
    if (FAILED(hr)) goto done;

    // Always 48 kHz stereo float; WASAPI converts from whatever the device or app uses.
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = AUDIO_RATE;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = AUDIO_FRAME_BYTES;
    fmt.nAvgBytesPerSec = AUDIO_RATE * AUDIO_FRAME_BYTES;
    flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (w->kind == AUDIO_APP) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    what = "initializing the capture";
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000 /* 200 ms */, 0, &fmt, 0);
    if (FAILED(hr)) goto done;
    ready = CreateEventW(0, FALSE, FALSE, 0);
    hr = client->SetEventHandle(ready);
    if (FAILED(hr)) goto done;
    hr = client->GetService(__uuidof(IAudioCaptureClient), (void **)&capture);
    if (FAILED(hr)) goto done;
    what = "starting the capture";
    hr = client->Start();
    if (FAILED(hr)) goto done;

    what = "capturing";
    while (!w->stop) {
        if (w->kind == AUDIO_MIC && !w->path[0] && GetTickCount64() - mic_checked >= 3000) {
            mic_checked = GetTickCount64();
            wchar_t now[256];
            default_mic_id(now, COUNT(now));
            if (now[0] && wcscmp(now, mic_id)) {
                log_line("the default mic changed; switching");
                end = CAPTURE_RESTART;
                goto done;
            }
        }
        bool got_event = WaitForSingleObject(ready, 200) == WAIT_OBJECT_0;
        bool any = false;
        for (;;) {
            UINT32 next = 0;
            hr = capture->GetNextPacketSize(&next);
            if (FAILED(hr)) goto done;
            if (!next) break;
            BYTE *data = 0;
            UINT32 frames = 0;
            DWORD buf_flags = 0;
            UINT64 qpc = 0;
            hr = capture->GetBuffer(&data, &frames, &buf_flags, 0, &qpc);
            if (FAILED(hr)) goto done;
            u32 bytes = frames * AUDIO_FRAME_BYTES;
            if (bytes > AUDIO_PACKET_CAP) {
                if (!warned_big) log_line("%s: a %u-frame audio packet was cut to 1 s", w->exe.s, frames);
                warned_big = true;
                bytes = AUDIO_PACKET_CAP;
            }
            if (buf_flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(packet, 0, bytes);
            else memcpy(packet, data, bytes);
            capture->ReleaseBuffer(frames);

            i64 t_us = qpc > 0 && !(buf_flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
                           ? qpc_to_wall_us(qpc)
                           : now_us() - (i64)(bytes / AUDIO_FRAME_BYTES) * 1000000 / AUDIO_RATE;
            if (!writer.open) {
                what = "starting the encoder";
                hr = E_FAIL;
                if (!writer_open(&writer, w->kind, w->exe.s, w->path, w->pid, w->buffer_dir, w->ffmpeg, t_us)) goto done;
                what = "capturing";
                hr = S_OK;
            }
            if (!writer_place(&writer, packet, bytes, t_us)) {
                what = "writing to the encoder (it stopped)";
                hr = E_FAIL;
                goto done;
            }
            any = true;
        }
        if (!any && !got_event && writer.open && !writer_idle(&writer)) {
            what = "writing to the encoder (it stopped)";
            hr = E_FAIL;
            goto done;
        }
        writer_drain_log(&writer);
    }
    end = CAPTURE_STOP;

done:
    if (end == CAPTURE_ERROR) log_line("%s: audio capture stopped while %s (0x%08x)", w->exe.s, what, (u32)hr);
    if (client) client->Stop();
    writer_finish(&writer, w->exe.s);
    if (capture) capture->Release();
    if (client) client->Release();
    if (ready) CloseHandle(ready);
    if (dev) dev->Release();
    if (en) en->Release();
    return end;
}

static DWORD WINAPI audio_worker_thread(void *arg)
{
    AudioWorker *w = (AudioWorker *)arg;
    CoInitializeEx(0, COINIT_MULTITHREADED);
    u8 *packet = (u8 *)VirtualAlloc(0, AUDIO_PACKET_CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(packet, "couldn't allocate an audio packet buffer (%d)", (int)GetLastError());
    while (!w->stop) {
        CaptureEnd end = audio_capture(w, packet);
        if (end == CAPTURE_STOP) break;
        if (end == CAPTURE_RESTART) continue;
        // An app's stream just ends; the watcher starts a new one if the app plays again. The mic retries every
        // 5 s (a headset unplugged with no other mic, say).
        if (w->kind == AUDIO_APP) break;
        for (i32 i = 0; i < 50 && !w->stop; i++) Sleep(100);
    }
    VirtualFree(packet, 0, MEM_RELEASE);
    CoUninitialize();
    InterlockedExchange(&w->finished, 1);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The watcher

struct AudioProc {
    u32   pid, parent;
    Str64 exe; // lower case
};

struct AudioProcList {
    AudioProc items[8192];
    i32       count;
};

struct AudioWatch {
    // Handed over by the main thread (audio_config_changed, audio_set_paused).
    SRWLOCK       lock;
    Config        pending;
    volatile LONG pending_set;
    volatile LONG paused;
    HANDLE        stop_event;
    HANDLE        thread;

    // The watcher thread's own.
    Config        config;
    Path          ffmpeg;
    bool          ffmpeg_missing_logged;
    AudioProcList procs;
    AudioWorker   workers[COUNT(((Audio *)0)->streams)]; // as many as the UI can list
    u32           next_id;
    bool          activity_started;
    Str64         game;
    Path          activity_file;
    Str64         focus;         // the program in front, as last written to focus.tsv
    Path          focus_file;
    HWND          window;        // the game window being recorded, 0 = the monitor
    Str64         window_exe;    // whose it is
    char          window_path[MAX_PATH];
};

static AudioWatch g_aw;

static void procs_snapshot(AudioProcList *list)
{
    list->count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W e = {};
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok && list->count < COUNT(list->items); ok = Process32NextW(snap, &e)) {
        AudioProc *p = &list->items[list->count++];
        p->pid = e.th32ProcessID;
        p->parent = e.th32ParentProcessID;
        CharLowerBuffW(e.szExeFile, (DWORD)wcslen(e.szExeFile));
        char exe[MAX_PATH * 3];
        wide_to_utf8(e.szExeFile, exe, sizeof exe);
        str_set(p->exe.s, exe);
    }
    CloseHandle(snap);
}

static const AudioProc *procs_find(const AudioProcList &list, u32 pid)
{
    for (i32 i = 0; i < list.count; i++)
        if (list.items[i].pid == pid) return &list.items[i];
    return 0;
}

// Browsers and launchers play sound from a child process. Process loopback captures a whole tree, so climb to the
// topmost ancestor that is still the same program.
static u32 procs_root_of(const AudioProcList &list, u32 pid)
{
    const AudioProc *p = procs_find(list, pid);
    if (!p) return pid;
    u32 root = pid;
    for (i32 i = 0; i < 32; i++) {
        const AudioProc *parent = procs_find(list, p->parent);
        if (!parent || parent->pid == root || strcmp(parent->exe.s, p->exe.s)) break;
        root = parent->pid;
        p = parent;
    }
    return root;
}

// PIDs with an audio session playing right now, on any output device.
static i32 audio_playing_pids(u32 *out, i32 cap)
{
    i32 n = 0;
    IMMDeviceEnumerator *en = device_enumerator();
    if (!en) return 0;
    IMMDeviceCollection *devices = 0;
    UINT device_count = 0;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) devices->GetCount(&device_count);
    for (UINT i = 0; i < device_count; i++) {
        IMMDevice *dev = 0;
        IAudioSessionManager2 *manager = 0;
        IAudioSessionEnumerator *sessions = 0;
        int session_count = 0;
        if (FAILED(devices->Item(i, &dev))) continue;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, 0, (void **)&manager)) &&
            SUCCEEDED(manager->GetSessionEnumerator(&sessions)))
            sessions->GetCount(&session_count);
        for (int j = 0; j < session_count; j++) {
            IAudioSessionControl *control = 0;
            if (FAILED(sessions->GetSession(j, &control))) continue;
            AudioSessionState state = AudioSessionStateInactive;
            IAudioSessionControl2 *control2 = 0;
            if (SUCCEEDED(control->GetState(&state)) && state == AudioSessionStateActive &&
                SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), (void **)&control2))) {
                DWORD pid = 0;
                // A session shared by several processes still names one of them (AUDCLNT_S_NO_SINGLE_PROCESS).
                if (SUCCEEDED(control2->GetProcessId(&pid))) {
                    bool seen = false;
                    for (i32 k = 0; k < n; k++) seen |= out[k] == pid;
                    if (!seen && n < cap) out[n++] = pid;
                }
                control2->Release();
            }
            control->Release();
        }
        if (sessions) sessions->Release();
        if (manager) manager->Release();
        dev->Release();
    }
    if (devices) devices->Release();
    en->Release();
    return n;
}

static AudioWorker *worker_start(AudioKind kind, const char *exe, const char *path, u32 pid)
{
    if (!g_aw.ffmpeg.s[0]) {
        if (!g_aw.ffmpeg_missing_logged) log_line("audio: ffmpeg wasn't found, so audio isn't recorded");
        g_aw.ffmpeg_missing_logged = true;
        return 0;
    }
    AudioWorker *w = 0;
    for (i32 i = 0; i < COUNT(g_aw.workers) && !w; i++)
        if (!g_aw.workers[i].used) w = &g_aw.workers[i];
    if (!w) {
        log_line("audio: already %d streams; %s isn't recorded", COUNT(g_aw.workers), exe);
        return 0;
    }
    *w = {};
    w->used = true;
    w->id = ++g_aw.next_id;
    w->kind = kind;
    str_set(w->exe.s, exe);
    str_set(w->path, path);
    w->pid = pid;
    w->buffer_dir = g_aw.config.buffer_dir;
    w->ffmpeg = g_aw.ffmpeg;
    w->last_playing_us = now_us();
    w->thread = thread_start(audio_worker_thread, w);

    Event e = {};
    e.kind = EV_AUDIO;
    e.a = AE_STARTED;
    e.b = (i64)w->id | ((i64)kind << 32);
    snprintf(e.text, sizeof e.text, "%s	%s", exe, path);
    event_post(e);
    return w;
}

// Stopping is two steps so several streams can finish their ffmpeg at the same time.
static void worker_stop_begin(AudioWorker *w)
{
    InterlockedExchange(&w->stop, 1);
}

static void worker_join(AudioWorker *w)
{
    WaitForSingleObject(w->thread, INFINITE);
    CloseHandle(w->thread);
    Event e = {};
    e.kind = EV_AUDIO;
    e.a = AE_STOPPED;
    e.b = w->id;
    event_post(e);
    *w = {};
}

static void workers_stop_all()
{
    for (i32 i = 0; i < COUNT(g_aw.workers); i++)
        if (g_aw.workers[i].used) worker_stop_begin(&g_aw.workers[i]);
    for (i32 i = 0; i < COUNT(g_aw.workers); i++)
        if (g_aw.workers[i].used) worker_join(&g_aw.workers[i]);
}

// text = "exe\tpath"
static void post_heard(const char *exe, const char *path, i64 us)
{
    Event e = {};
    e.kind = EV_AUDIO;
    e.a = AE_HEARD;
    e.b = us;
    snprintf(e.text, sizeof e.text, "%s\t%s", exe, path);
    event_post(e);
}

// One second's work: start streams for apps playing sound, close the ones gone quiet.
static void audio_tick_streams()
{
    const Config &cfg = g_aw.config;
    i64 now = now_us();

    AudioWorker *mic = 0;
    for (i32 i = 0; i < COUNT(g_aw.workers); i++)
        if (g_aw.workers[i].used && g_aw.workers[i].kind == AUDIO_MIC && !g_aw.workers[i].path[0]) mic = &g_aw.workers[i];
    bool mic_on = mic_for(cfg, g_aw.game.s); // a game can have the Mic on or off over it
    if (mic && (!mic_on || mic->finished)) {
        worker_stop_begin(mic);
        worker_join(mic);
        mic = 0;
    }
    if (mic_on && !mic) worker_start(AUDIO_MIC, "mic", "", 0);

    // Other inputs turned on, while plugged in. One that is Windows' default is already on the Mic track, so it
    // waits until it no longer is.
    InputDevice live[32];
    char default_id[128];
    i32 live_count = cfg.input_count ? audio_list_inputs(live, COUNT(live), default_id, sizeof default_id) : 0;
    auto wanted = [&](const char *id) {
        const InputDevice *d = config_input(cfg.inputs, cfg.input_count, id);
        if (!d || d->mode == INPUT_OFF || (mic_on && !_stricmp(id, default_id))) return (const InputDevice *)0;
        for (i32 i = 0; i < live_count; i++)
            if (!_stricmp(live[i].id, id)) return d;
        return (const InputDevice *)0;
    };
    for (i32 i = 0; i < COUNT(g_aw.workers); i++) {
        AudioWorker *w = &g_aw.workers[i];
        if (!w->used || w->kind != AUDIO_MIC || !w->path[0] || (wanted(w->path) && !w->finished)) continue;
        log_line("input %s closed", w->exe.s);
        worker_stop_begin(w);
        worker_join(w);
    }
    for (i32 i = 0; i < cfg.input_count; i++) {
        const InputDevice *d = wanted(cfg.inputs[i].id);
        if (!d) continue;
        bool running = false;
        for (i32 k = 0; k < COUNT(g_aw.workers) && !running; k++)
            running = g_aw.workers[k].used && g_aw.workers[k].kind == AUDIO_MIC && !_stricmp(g_aw.workers[k].path, d->id);
        if (!running) worker_start(AUDIO_MIC, d->name, d->id, 0);
    }

    // Close streams whose app exited, is now ignored, failed, or has been quiet too long.
    bool closing[COUNT(g_aw.workers)] = {};
    for (i32 i = 0; i < COUNT(g_aw.workers); i++) {
        AudioWorker *w = &g_aw.workers[i];
        if (!w->used || w->kind != AUDIO_APP) continue;
        const AudioProc *p = procs_find(g_aw.procs, w->pid);
        bool alive = p && !strcmp(p->exe.s, w->exe.s);
        bool keep = alive && audio_category_at(cfg, w->exe.s, w->path) != CAT_IGNORED && !w->finished &&
                    now - w->last_playing_us < AUDIO_LINGER_US;
        if (!keep) {
            log_line("%s audio stream closed", w->exe.s);
            worker_stop_begin(w);
            closing[i] = true;
        }
    }
    for (i32 i = 0; i < COUNT(g_aw.workers); i++)
        if (closing[i]) worker_join(&g_aw.workers[i]);

    u32 playing[256];
    i32 playing_count = audio_playing_pids(playing, COUNT(playing));
    u32 own_pid = GetCurrentProcessId();
    for (i32 i = 0; i < playing_count; i++) {
        u32 pid = playing[i];
        if (pid == 0 || pid == own_pid) continue; // 0 is "system sounds"
        const AudioProc *p = procs_find(g_aw.procs, pid);
        if (!p || !p->exe.s[0]) continue;
        // Rekodr's own processes never: its window plays clips back.
        if (starts_with(p->exe.s, "rekodr")) continue;
        u32 root = procs_root_of(g_aw.procs, pid);
        AudioWorker *running = 0;
        for (i32 k = 0; k < COUNT(g_aw.workers) && !running; k++) {
            AudioWorker *w = &g_aw.workers[k];
            if (w->used && w->kind == AUDIO_APP && w->pid == root) running = w;
        }
        if (!running) {
            // Decided by where it runs from, before anything of it is kept: an ignored folder's app is never opened.
            SeenProc *seen = seen_proc(pid, p->exe.s);
            if (audio_category_at(cfg, p->exe.s, seen->path) == CAT_IGNORED) {
                // Not recorded, but still listed in Options with where it runs from
                if (!seen->heard_us || now - seen->heard_us > 60000000) {
                    seen->heard_us = now;
                    post_heard(p->exe.s, seen->path, now);
                }
                continue;
            }
            log_line("recording %s audio (pid %u)", p->exe.s, root);
            running = worker_start(AUDIO_APP, p->exe.s, seen->path, root);
            if (!running) continue;
        }
        running->last_playing_us = now;
        // "Last heard" for Options (and new apps for it to list), at most once a minute per stream.
        if (!running->last_heard_us || now - running->last_heard_us > 60000000) {
            running->last_heard_us = now;
            post_heard(running->exe.s, running->path, now);
        }
    }
}

// Whether a game's window is recorded instead of the monitor: its own setting, else Config.window_capture.
static bool window_capture_for(const Config &cfg, const char *exe, const char *path)
{
    if (audio_category_at(cfg, exe, path) != CAT_GAME) return false;
    for (i32 i = 0; i < cfg.app_count; i++)
        if (!strcmp(cfg.apps[i].exe.s, exe) && cfg.apps[i].rec.window) return cfg.apps[i].rec.window > 0;
    return cfg.window_capture;
}

// "<us>\t<text>\n" added to a log in the buffer folder (activity.tsv, focus.tsv).
static void buffer_log_append(const Path &file, const char *text)
{
    AcquireSRWLockExclusive(&g_activity_lock);
    HANDLE f = CreateFileW(file.s, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, 0);
    if (f != INVALID_HANDLE_VALUE) {
        char line[128];
        int n = snprintf(line, sizeof line, "%lld\t%s\n", (long long)now_us(), text);
        DWORD written;
        WriteFile(f, line, (DWORD)n, &written, 0);
        CloseHandle(f);
    } else {
        log_line("couldn't write %ls (%d)", file.s, (int)GetLastError());
    }
    ReleaseSRWLockExclusive(&g_activity_lock);
}

// Every 2 s: the game whose window is in front, else the last one played if it's still running, else any running
// game (the first by name), else the desktop. Changes are appended to activity.tsv. What is in front, whatever it
// is, goes to focus.tsv the same way. And the window to record is kept up: the last game in front that has
// "capture window" on, for as long as that window is there and not minimised.
static void audio_tick_activity()
{
    const Config &cfg = g_aw.config;
    Str64 game = {};

    DWORD fg_pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &fg_pid);
    const AudioProc *front = fg_pid ? procs_find(g_aw.procs, fg_pid) : 0;
    char front_path[MAX_PATH] = "";
    if (fg_pid) proc_image_path(fg_pid, front_path, sizeof front_path);

    // focus.tsv: a line when the program in front changes (and a fresh one on start, or when the buffer moves)
    {
        Str64 focus = {};
        const char *name = strrchr(front_path, '\\');
        str_set(focus.s, name ? name + 1 : front_path);
        for (char *p = focus.s; *p; p++) *p = (char)tolower((u8)*p);
        Path file = path_join(cfg.buffer_dir, L"focus.tsv");
        if (!str_eq(focus.s, g_aw.focus.s) || wcscmp(file.s, g_aw.focus_file.s)) {
            SHCreateDirectoryExW(0, cfg.buffer_dir.s, 0);
            buffer_log_append(file, focus.s);
            g_aw.focus = focus;
            g_aw.focus_file = file;
        }
    }

    // The window to record
    {
        HWND want = g_aw.window;
        if (front && fg && window_capture_for(cfg, front->exe.s, front_path)) {
            want = fg;
            g_aw.window_exe = front->exe;
            str_set(g_aw.window_path, front_path);
        } else if (want && (!IsWindow(want) || IsIconic(want) || !window_capture_for(cfg, g_aw.window_exe.s, g_aw.window_path))) {
            want = 0; // gone, minimised, or its setting was turned off: the monitor again
        }
        if (want != g_aw.window) {
            g_aw.window = want;
            if (want) log_line("recording the window of %s", g_aw.window_exe.s);
            else log_line("recording the monitor");
            Event e = {};
            e.kind = EV_AUDIO;
            e.a = AE_WINDOW;
            e.b = (i64)(intptr_t)want;
            event_post(e);
        }
    }

    if (front && audio_category_at(cfg, front->exe.s, front_path) == CAT_GAME) {
        game = front->exe;
    } else {
        bool prev_running = false;
        const char *first = 0;
        for (i32 i = 0; i < cfg.app_count; i++) {
            const AppInfo &a = cfg.apps[i];
            if (app_category_in(cfg.apps, cfg.app_count, cfg.folders, cfg.folder_count, a.exe.s, a.path) != CAT_GAME) continue;
            bool running = false;
            for (i32 k = 0; k < g_aw.procs.count && !running; k++) running = !strcmp(g_aw.procs.items[k].exe.s, a.exe.s);
            if (!running) continue;
            if (str_eq(g_aw.game.s, a.exe.s)) prev_running = true;
            if (!first || strcmp(a.exe.s, first) < 0) first = a.exe.s;
        }
        if (prev_running) game = g_aw.game;
        else if (first) str_set(game.s, first);
    }

    Path file = path_join(cfg.buffer_dir, L"activity.tsv");
    // A fresh line on start (and when the buffer moves) so the file always says what was on.
    if (g_aw.activity_started && str_eq(game.s, g_aw.game.s) && !wcscmp(file.s, g_aw.activity_file.s)) return;

    SHCreateDirectoryExW(0, cfg.buffer_dir.s, 0);
    buffer_log_append(file, game.s);
    if (g_aw.activity_started) {
        if (game.s[0]) {
            char name[128];
            audio_display_name(cfg, game.s, name, sizeof name);
            log_line("playing %s", name);
        } else {
            log_line("on the desktop");
        }
    }
    g_aw.activity_started = true;
    g_aw.game = game;
    g_aw.activity_file = file;

    Event e = {};
    e.kind = EV_AUDIO;
    e.a = AE_GAME;
    str_set(e.text, game.s);
    event_post(e);
}

static DWORD WINAPI audio_watch_thread(void *)
{
    CoInitializeEx(0, COINIT_MULTITHREADED);
    bool was_paused = false;
    for (u64 tick = 0;; tick++) {
        if (g_aw.pending_set) {
            AcquireSRWLockExclusive(&g_aw.lock);
            memcpy(&g_aw.config, &g_aw.pending, sizeof g_aw.config);
            InterlockedExchange(&g_aw.pending_set, 0);
            ReleaseSRWLockExclusive(&g_aw.lock);
            Path found;
            if (ffmpeg_find(g_aw.config.ffmpeg, &found)) {
                g_aw.ffmpeg = found;
                g_aw.ffmpeg_missing_logged = false;
            } else {
                g_aw.ffmpeg = {};
            }
        }
        procs_snapshot(&g_aw.procs);
        if (tick % 2 == 0) audio_tick_activity();
        bool paused = g_aw.paused != 0;
        if (paused && !was_paused) workers_stop_all();
        if (!paused) audio_tick_streams();
        was_paused = paused;
        if (WaitForSingleObject(g_aw.stop_event, 1000) != WAIT_TIMEOUT) break;
    }
    workers_stop_all();
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Main thread

// Hands the watcher a copy of the settings. Call it whenever Config changes (Options does; so does a new app heard).
static void audio_config_changed()
{
    AcquireSRWLockExclusive(&g_aw.lock);
    memcpy(&g_aw.pending, &g_app.config, sizeof g_aw.pending);
    InterlockedExchange(&g_aw.pending_set, 1);
    ReleaseSRWLockExclusive(&g_aw.lock);
    Audio &a = g_app.audio;
    for (i32 i = 0; i < a.stream_count; i++)
        a.streams[i].track = audio_track(g_app.config, a.streams[i].kind, a.streams[i].exe.s, a.streams[i].path);
}

static void audio_start()
{
    InitializeSRWLock(&g_aw.lock);
    audio_config_changed();
    g_aw.stop_event = CreateEventW(0, TRUE, FALSE, 0);
    g_aw.thread = thread_start(audio_watch_thread, 0);
}

// Stops every stream, letting each ffmpeg finish its last segment.
static void audio_stop()
{
    if (!g_aw.thread) return;
    SetEvent(g_aw.stop_event);
    WaitForSingleObject(g_aw.thread, INFINITE);
    CloseHandle(g_aw.thread);
    CloseHandle(g_aw.stop_event);
    g_aw.thread = 0;
}

// Pausing stops every stream (what's recorded stays in the buffer); resuming starts them again as apps play.
static void audio_set_paused(bool paused)
{
    InterlockedExchange(&g_aw.paused, paused ? 1 : 0);
}

static void hotkeys_apply(const Config &c, HWND hwnd, const char *game); // hotkeys.cpp

static void audio_on_event(const Event &e)
{
    Audio &a = g_app.audio;
    switch ((AudioEventKind)e.a) {
    case AE_STARTED: {
        // There are as many workers as slots here, so this only fails if events were dropped.
        if (a.stream_count == COUNT(a.streams)) break;
        AudioLive *s = &a.streams[a.stream_count++];
        *s = {};
        s->id = (u32)(e.b & 0xffffffff);
        s->kind = (AudioKind)(e.b >> 32);
        split_exe_path(e.text, &s->exe, s->path, sizeof s->path);
        s->track = audio_track(g_app.config, s->kind, s->exe.s, s->path);
        break;
    }
    case AE_STOPPED:
        for (i32 i = 0; i < a.stream_count; i++) {
            if (a.streams[i].id != (u32)e.b) continue;
            memmove(&a.streams[i], &a.streams[i + 1], (a.stream_count - i - 1) * sizeof a.streams[0]);
            a.stream_count--;
            break;
        }
        break;
    case AE_HEARD: {
        Str64 exe;
        char path[MAX_PATH];
        split_exe_path(e.text, &exe, path, sizeof path);
        i32 before = g_app.config.app_count;
        AppInfo *app = config_app(&g_app.config, exe.s); // a new one is CAT_NONE: shown as new in Options
        if (!app) break; // the table is full
        app->last_heard_us = e.b;
        bool moved = path[0] && strcmp(app->path, path);
        if (path[0]) str_set(app->path, path);
        if (moved || (path[0] && g_app.config.app_count != before)) // its name can differ where it runs from now
            exe_description(app->path, app->desc.s, sizeof app->desc.s);
        bool named = (moved || g_app.config.app_count != before) && gamedb_name_app(app); // a known game: its name, and sorted
        if (g_app.config.app_count != before) {
            log_line("new app heard: %s", exe.s);
            audio_config_changed();
        } else if (moved) {
            audio_config_changed(); // the watcher detects games by it
        }
        if (named && !moved && g_app.config.app_count == before) audio_config_changed();
        if (moved || named) config_save(g_app.config, g_app.paths); // folder rules decide by it, so keep it
        break;
    }
    case AE_GAME:
        str_set(a.game.s, e.text);
        // An exe several games share (javaw.exe) is whichever one is running now, so ask while it is
        for (i32 i = 0; e.text[0] && i < g_app.config.app_count; i++)
            if (!strcmp(g_app.config.apps[i].exe.s, e.text) && gamedb_name_app(&g_app.config.apps[i])) config_save(g_app.config, g_app.paths);
        recorder_settings_changed(); // a game can have its own recording settings
        // And its own hotkeys, unless Options is taking a hotkey's keys (they're all off meanwhile)
        if (!is_test_copy() && !g_app.options.capture_slot) hotkeys_apply(g_app.config, g_app.msg_hwnd, a.game.s);
        sessions_on_game(e.text);
        break;
    case AE_WINDOW:
        a.window = (u64)e.b;
        recorder_settings_changed(); // a new capture, of that window or of the monitor
        break;
    default: break;
    }
}
