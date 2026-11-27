// Test, in a mic's Sound panel: MIC_TEST_S seconds from that mic, saved as a WAV, then played as recorded (Before)
// and through its sound (After, made by ffmpeg with mic_chain). After is made again whenever the sound changes. A
// test copy that may not capture (capture_allowed) hears a made-up voice instead of the mic.

static const i32 MIC_TEST_S = 5;
enum MicTestState : LONG { MT_IDLE, MT_RECORDING, MT_RENDERING, MT_READY, MT_FAILED };

struct MicTest { // the panel's thread starts and ends it; its own thread records and makes After
    volatile LONG state;
    volatile LONG stop;
    volatile f32 progress, level; // 0..1, for the bar
    volatile LONG playing;        // -1 none, 0 Before, 1 After
    volatile LONG play_stop;
    volatile LONG autoplay;       // set when After is made: 1 = play Before then After, 2 = After; the UI starts it
    char id[128];                 // the device, "" = Windows' default
    bool fake, fast;              // fast: not paced to the clock (tests)
    Path ffmpeg, before, after;
    MicSound sound;               // what After is made with
    HANDLE thread, player;
};
static MicTest g_mic_test = {MT_IDLE, 0, 0, 0, -1};

// A made-up voice: a hum of harmonics on a wavering pitch, in syllables, over a little hiss and mains hum (so Clean
// and the EQ have something to change).
static void mic_test_fake(f32 *out, i32 frames)
{
    const f64 pi = 3.14159265358979323846;
    u32 seed = 12345;
    f64 phase = 0;
    for (i32 i = 0; i < frames; i++) {
        f64 t = (f64)i / AUDIO_RATE;
        f64 f0 = 140 + 25 * sin(2 * pi * 0.7 * t) + 8 * sin(2 * pi * 5 * t);
        phase += 2 * pi * f0 / AUDIO_RATE;
        f64 v = 0;
        for (i32 h = 1; h <= 12; h++) v += sin(phase * h) / h;
        f64 syll = fmax(0, sin(2 * pi * 2.2 * t)) * (fmod(t, 1.6) < 1.2 ? 1 : 0);
        seed = seed * 1664525 + 1013904223;
        f64 hiss = ((seed >> 9) / 4194304.0 - 1) * 0.01;
        f64 s = 0.25 * syll * v + hiss + 0.02 * sin(2 * pi * 50 * t);
        out[2 * i] = out[2 * i + 1] = (f32)s;
    }
}

// 16-bit stereo WAV at AUDIO_RATE from float frames.
static bool mic_test_wav_write(const wchar_t *path, const f32 *frames, i32 n)
{
    u32 data = (u32)n * 4;
    u8 *b = (u8 *)malloc(44 + data);
    if (!b) return false;
    auto u32le = [&](i32 at, u32 v) { memcpy(b + at, &v, 4); };
    auto u16le = [&](i32 at, u16 v) { memcpy(b + at, &v, 2); };
    memcpy(b, "RIFF", 4), u32le(4, 36 + data), memcpy(b + 8, "WAVEfmt ", 8);
    u32le(16, 16), u16le(20, 1), u16le(22, 2), u32le(24, AUDIO_RATE), u32le(28, AUDIO_RATE * 4), u16le(32, 4), u16le(34, 16);
    memcpy(b + 36, "data", 4), u32le(40, data);
    i16 *pcm = (i16 *)(b + 44);
    for (i32 i = 0; i < 2 * n; i++) pcm[i] = (i16)lrintf(fmaxf(-1, fminf(1, frames[i])) * 32767);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    DWORD wrote = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, b, 44 + data, &wrote, 0) && wrote == 44 + data;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(b);
    return ok;
}

// After from Before: the sound's filters, through ffmpeg.
static bool mic_test_render(const Path &ffmpeg, const Path &before, const MicSound &s, const Path &after)
{
    char chain[512];
    mic_chain(s, chain, sizeof chain);
    const char *a[] = {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i"};
    const char *b[] = {"-af", chain, "-c:a", "pcm_s16le"};
    wchar_t args[2048] = {};
    arg_list(args, COUNT(args), a, COUNT(a));
    cmd_arg(args, COUNT(args), before.s);
    wchar_t more[1024] = {};
    arg_list(more, COUNT(more), b, COUNT(b));
    wcscat_s(args, L" ");
    wcscat_s(args, more);
    cmd_arg(args, COUNT(args), after.s);
    return proc_run(ffmpeg.s, args, 30000, 0, 0) == 0;
}

// The mic's frames for MIC_TEST_S seconds, 48 kHz stereo float as Windows gives them. False if it can't be opened.
static bool mic_test_capture(MicTest *t, f32 *out, i32 frames)
{
    CoInitializeEx(0, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = device_enumerator();
    IMMDevice *dev = 0;
    IAudioClient *client = 0;
    IAudioCaptureClient *capture = 0;
    bool ok = false;
    i32 got = 0;
    HRESULT hr = E_FAIL;
    if (en) {
        if (t->id[0]) {
            wchar_t want[256];
            utf8_to_wide(t->id, want, COUNT(want));
            hr = en->GetDevice(want, &dev);
        } else hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
    }
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void **)&client);
    if (SUCCEEDED(hr)) {
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT, fmt.nChannels = 2, fmt.nSamplesPerSec = AUDIO_RATE;
        fmt.wBitsPerSample = 32, fmt.nBlockAlign = AUDIO_FRAME_BYTES, fmt.nAvgBytesPerSec = AUDIO_RATE * AUDIO_FRAME_BYTES;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                2000000, 0, &fmt, 0);
    }
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), (void **)&capture);
    if (SUCCEEDED(hr)) hr = client->Start();
    if (SUCCEEDED(hr)) {
        ok = true;
        while (got < frames && !t->stop) {
            Sleep(20);
            UINT32 next = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&next)) && next) {
                BYTE *data = 0;
                UINT32 n = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &n, &flags, 0, 0))) break;
                i32 take = (i32)n < frames - got ? (i32)n : frames - got;
                f32 peak = 0;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(out + 2 * got, 0, take * AUDIO_FRAME_BYTES);
                else memcpy(out + 2 * got, data, take * AUDIO_FRAME_BYTES);
                for (i32 i = 0; i < 2 * take; i++) peak = fmaxf(peak, fabsf(out[2 * got + i]));
                got += take;
                capture->ReleaseBuffer(n);
                t->level = fmaxf(peak, t->level * 0.8f);
            }
            t->progress = (f32)got / frames;
        }
        client->Stop();
        memset(out + 2 * got, 0, (frames - got) * AUDIO_FRAME_BYTES);
    }
    if (capture) capture->Release();
    if (client) client->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    CoUninitialize();
    if (!ok) log_line("mic test: couldn't open the mic (0x%08x)", (u32)hr);
    return ok;
}

static void mic_test_play_stop()
{
    MicTest &t = g_mic_test;
    if (!t.player) return;
    t.play_stop = 1;
    PlaySoundW(0, 0, 0);
    WaitForSingleObject(t.player, 3000);
    CloseHandle(t.player);
    t.player = 0;
    t.playing = -1;
}

// Plays from Before (0) or After (1) up to `to`.
struct MicTestPlay { i32 from, to; };
static DWORD WINAPI mic_test_play_thread(void *arg)
{
    MicTest &t = g_mic_test;
    MicTestPlay p = *(MicTestPlay *)&arg;
    for (i32 k = p.from; k <= p.to && !t.play_stop; k++) {
        t.playing = k;
        PlaySoundW(k ? t.after.s : t.before.s, 0, SND_FILENAME | SND_SYNC | SND_NODEFAULT);
    }
    t.playing = -1;
    return 0;
}

static void mic_test_play(i32 from, i32 to)
{
    MicTest &t = g_mic_test;
    if (t.state != MT_READY) return;
    mic_test_play_stop();
    t.play_stop = 0;
    static_assert(sizeof(MicTestPlay) <= sizeof(void *), "MicTestPlay rides in the thread's argument");
    void *arg = 0;
    MicTestPlay p = {from, to};
    memcpy(&arg, &p, sizeof p);
    t.player = CreateThread(0, 0, mic_test_play_thread, arg, 0, 0);
}

// Records (or makes up) Before, then makes After and plays both.
static DWORD WINAPI mic_test_thread(void *arg)
{
    MicTest &t = g_mic_test;
    bool render_only = arg != 0;
    if (!render_only) {
        i32 frames = MIC_TEST_S * AUDIO_RATE;
        f32 *buf = (f32 *)calloc((size_t)frames * 2, sizeof(f32));
        bool ok = buf != 0;
        if (ok && t.fake) {
            mic_test_fake(buf, frames);
            for (i32 step = 1; step <= 50 && !t.stop; step++) { // paced like a real one
                if (!t.fast) Sleep(MIC_TEST_S * 1000 / 50);
                f32 peak = 0;
                for (i32 i = (step - 1) * frames / 50; i < step * frames / 50; i++) peak = fmaxf(peak, fabsf(buf[2 * i]));
                t.level = peak, t.progress = step / 50.f;
            }
        } else if (ok) ok = mic_test_capture(&t, buf, frames);
        ok = ok && !t.stop && mic_test_wav_write(t.before.s, buf, frames);
        free(buf);
        t.level = 0;
        if (!ok) {
            InterlockedExchange(&t.state, t.stop ? MT_IDLE : MT_FAILED);
            return 0;
        }
    }
    InterlockedExchange(&t.state, MT_RENDERING);
    bool ok = t.ffmpeg.s[0] && mic_test_render(t.ffmpeg, t.before, t.sound, t.after);
    if (!ok) log_line("mic test: ffmpeg couldn't make After");
    InterlockedExchange(&t.state, ok ? MT_READY : MT_FAILED);
    if (ok && !t.stop) t.autoplay = render_only ? 2 : 1;
    return 0;
}

static void mic_test_join()
{
    MicTest &t = g_mic_test;
    if (!t.thread) return;
    WaitForSingleObject(t.thread, 35000);
    CloseHandle(t.thread);
    t.thread = 0;
}

// Ends a test: stops what records or plays, and deletes its files.
static void mic_test_end()
{
    MicTest &t = g_mic_test;
    t.stop = 1;
    mic_test_play_stop();
    mic_test_join();
    if (t.before.s[0]) DeleteFileW(t.before.s);
    if (t.after.s[0]) DeleteFileW(t.after.s);
    t.state = MT_IDLE;
    t.progress = t.level = 0;
    t.stop = 0;
}

static void mic_test_start(const char *id, const MicSound &s, bool fake, const Path &ffmpeg, const Path &dir)
{
    MicTest &t = g_mic_test;
    mic_test_end();
    str_set(t.id, id ? id : "");
    t.fake = fake;
    t.sound = s;
    t.ffmpeg = ffmpeg;
    CreateDirectoryW(dir.s, 0);
    t.before = path_join(dir, L"mic-test-before.wav");
    t.after = path_join(dir, L"mic-test-after.wav");
    t.state = MT_RECORDING;
    log_line("mic test: %s", fake ? "a made-up voice (test copy)" : "recording");
    t.thread = CreateThread(0, 0, mic_test_thread, 0, 0, 0);
}

// The sound changed: After is made again, then played.
static void mic_test_resound(const MicSound &s)
{
    MicTest &t = g_mic_test;
    if (t.state != MT_READY && t.state != MT_FAILED) return;
    if (t.state == MT_FAILED && GetFileAttributesW(t.before.s) == INVALID_FILE_ATTRIBUTES) return;
    mic_test_play_stop();
    mic_test_join();
    t.sound = s;
    t.state = MT_RENDERING;
    t.thread = CreateThread(0, 0, mic_test_thread, (void *)1, 0, 0);
}
