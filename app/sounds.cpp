// Sounds: an event's sound (Config.sounds) played when it happens, and any sound played for Options' picker. The
// library's sounds are in the exe (RCDATA SOUND_RES + index); an own sound is a .wav read from its file, at most
// SOUND_OWN_MAX_BYTES, of which the first SOUND_OWN_MAX_S seconds play. Both play on PlaySound's own thread, so a
// new sound cuts off the one before.

static const i64 SOUND_OWN_MAX_BYTES = 5 * 1024 * 1024;
static const i32 SOUND_OWN_MAX_S = 10;

// A .wav in memory cut to its first SOUND_OWN_MAX_S seconds: the data chunk and the RIFF size are rewritten and
// *len is the cut file's length. *secs is the whole sound's length. False when it isn't a .wav with a format and
// a data chunk.
static bool sound_wav_cut(u8 *b, u32 n, u32 *len, f64 *secs)
{
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return false;
    u32 avg = 0, block = 1, data_at = 0, data_len = 0;
    for (u64 at = 12; at + 8 <= n;) {
        u32 size;
        memcpy(&size, b + at + 4, 4);
        if (!memcmp(b + at, "fmt ", 4) && size >= 14 && at + 8 + 14 <= n) {
            u16 align;
            memcpy(&avg, b + at + 16, 4);
            memcpy(&align, b + at + 20, 2);
            block = align ? align : 1;
        } else if (!memcmp(b + at, "data", 4)) {
            data_at = (u32)at + 8;
            data_len = size < n - data_at ? size : n - data_at;
            break;
        }
        at += 8 + (u64)size + (size & 1);
    }
    if (!avg || !data_at) return false;
    *secs = (f64)data_len / avg;
    u64 most = (u64)avg * SOUND_OWN_MAX_S / block * block;
    u32 keep = data_len < most ? data_len : (u32)most;
    memcpy(b + data_at - 4, &keep, 4);
    *len = data_at + keep;
    u32 riff = *len - 8;
    memcpy(b + 4, &riff, 4);
    return true;
}

// An own sound's file read whole and cut (sound_wav_cut), for free(). 0 with a one-line reason in `why` when it
// can't be read, is too big or isn't a .wav.
static u8 *sound_own_load(const Path &own, u32 *len, f64 *secs, char *why, size_t why_size)
{
    FILE *f = _wfopen(own.s, L"rb");
    if (!f) {
        snprintf(why, why_size, "Can't be read");
        return 0;
    }
    _fseeki64(f, 0, SEEK_END);
    i64 size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (size > SOUND_OWN_MAX_BYTES) {
        fclose(f);
        snprintf(why, why_size, "Too big: own sounds are up to %d MB", (i32)(SOUND_OWN_MAX_BYTES >> 20));
        return 0;
    }
    u8 *b = size > 0 ? (u8 *)malloc((size_t)size) : 0;
    u32 n = b ? (u32)fread(b, 1, (size_t)size, f) : 0;
    fclose(f);
    if (!b || !sound_wav_cut(b, n, len, secs)) {
        free(b);
        snprintf(why, why_size, "Not a .wav Windows can play");
        return 0;
    }
    return b;
}

// Whether a file can be picked as an own sound: a .wav of at most SOUND_OWN_MAX_BYTES and SOUND_OWN_MAX_S
// seconds. The reason in `why` when not.
static bool sound_own_check(const Path &file, char *why, size_t why_size)
{
    u32 len;
    f64 secs;
    u8 *b = sound_own_load(file, &len, &secs, why, why_size);
    if (!b) return false;
    free(b);
    if (secs > SOUND_OWN_MAX_S + 0.05) {
        snprintf(why, why_size, "Too long: own sounds are up to %d s", SOUND_OWN_MAX_S);
        return false;
    }
    return true;
}

// The own sound playing (PlaySound reads it from memory as it plays), replaced under the lock: events come from
// more than one thread.
static SRWLOCK g_sound_lock = SRWLOCK_INIT;
static u8 *g_sound_own;

// Plays a library sound, or `own` for SOUND_OWN. Nothing when the file can't be played.
static void sound_play_pick(u8 sound, const Path &own)
{
    if (sound == SOUND_OWN) {
        if (!own.s[0]) return;
        u32 len;
        f64 secs;
        char why[96];
        u8 *wav = sound_own_load(own, &len, &secs, why, sizeof why);
        if (!wav) {
            log_line("sounds: own sound not played: %s", why);
            return;
        }
        AcquireSRWLockExclusive(&g_sound_lock);
        PlaySoundW(0, 0, 0); // stops the one playing before its memory goes
        free(g_sound_own);
        g_sound_own = wav;
        PlaySoundW((LPCWSTR)wav, 0, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        ReleaseSRWLockExclusive(&g_sound_lock);
        return;
    }
    if (sound >= SOUND_COUNT) return;
    HRSRC res = FindResourceW(0, MAKEINTRESOURCEW(SOUND_RES + sound), MAKEINTRESOURCEW(10)); // RT_RCDATA
    HGLOBAL mem = res ? LoadResource(0, res) : 0;
    void *wav = mem ? LockResource(mem) : 0;
    if (wav) PlaySoundW((LPCWSTR)wav, 0, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// An event happened: its sound, when its switch is on.
static void sound_event(SoundEvent e)
{
    const SoundPick &p = g_app.config.sounds[e];
    if (p.on) sound_play_pick(p.sound, p.own);
}
