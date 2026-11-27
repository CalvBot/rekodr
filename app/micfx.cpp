// How a mic sounds in saves: its MicSound (Options' Sound panel, kept per device) as an ffmpeg filter chain, run on
// that mic's track when a clip saves, and the same chain's EQ curve for the panel to draw.
//
// The chain: Clean (rumble cut, then the steady noise floor taken down), the EQ (a low shelf, three peaks, a high
// shelf), the gain, then a gentle compressor. The EQ is RBJ biquads, as ffmpeg's lowshelf, equalizer and highshelf
// are, so mic_curve_db() is what the filters do.

static const f32 MIC_BAND_HZ[MIC_BANDS] = {100, 300, 1000, 3000, 8000};
static const f32 MIC_BAND_Q[MIC_BANDS] = {0.707f, 1, 1, 1, 0.707f};
static const f32 MIC_DB_MAX = 12; // each band and the gain go from -12 to +12 dB

// The Mic's cleaning: rumble below a voice cut, then the steady noise floor (fans, hiss, hum), which afftdn learns
// as it goes, taken down by 12 dB. Speech and short sounds are left as they are.
static const char MIC_CLEAN[] = "highpass=f=80,afftdn=nr=12:nf=-45:tn=1";
// Gentle: about 3:1 over -20 dB, a little made up after.
static const char MIC_COMPRESS[] = "acompressor=threshold=0.1:ratio=3:attack=10:release=150:makeup=1.5";

struct MicPreset { const char *name; f32 bands[MIC_BANDS]; };
static const MicPreset MIC_PRESETS[] = {
    {"Flat", {0, 0, 0, 0, 0}},
    {"Voice", {-4, -1, 0, 3, 1}},
    {"Bass cut", {-10, -4, 0, 0, 0}},
    {"Bright", {0, 0, 0, 3, 5}},
};

// A mic nobody has set: cleaned, nothing else.
static MicSound mic_sound_default(const char *id)
{
    MicSound s = {};
    if (id) str_set(s.id, id);
    s.clean = true;
    return s;
}

// The settings for device `id` (ignoring case, as Windows' ids vary), else the default.
static MicSound mic_sound_get(const MicSound *list, i32 n, const char *id)
{
    for (i32 i = 0; i < n; i++)
        if (id && id[0] && !_stricmp(list[i].id, id)) return list[i];
    return mic_sound_default(id);
}

// The settings for device `id` to change, added when it has none. 0 when the list is full.
static MicSound *mic_sound_edit(MicSound *list, i32 *n, i32 cap, const char *id)
{
    for (i32 i = 0; i < *n; i++)
        if (!_stricmp(list[i].id, id)) return &list[i];
    if (*n >= cap) return 0;
    list[*n] = mic_sound_default(id);
    return &list[(*n)++];
}

// The preset the bands are, -1 = none (Custom).
static i32 mic_preset_of(const MicSound &s)
{
    for (i32 p = 0; p < (i32)COUNT(MIC_PRESETS); p++) {
        bool same = true;
        for (i32 b = 0; b < MIC_BANDS; b++) same &= fabsf(s.bands[b] - MIC_PRESETS[p].bands[b]) < 0.05f;
        if (same) return p;
    }
    return -1;
}

// Whether the chain leaves the mic as it was recorded.
static bool mic_sound_plain(const MicSound &s)
{
    if (s.clean || s.comp || fabsf(s.gain_db) >= 0.05f) return false;
    for (i32 b = 0; b < MIC_BANDS; b++)
        if (fabsf(s.bands[b]) >= 0.05f) return false;
    return true;
}

// The ffmpeg filters for `s`, comma separated; "anull" when it changes nothing. Returns the length.
static i32 mic_chain(const MicSound &s, char *out, i32 cap)
{
    i32 n = 0;
    out[0] = 0;
    auto add = [&](const char *fmt, auto... args) {
        if (n && n < cap - 1) out[n++] = ',', out[n] = 0;
        if (n < cap) n += snprintf(out + n, cap - n, fmt, args...);
        if (n > cap - 1) n = cap - 1;
    };
    if (s.clean) add("%s", MIC_CLEAN);
    for (i32 b = 0; b < MIC_BANDS; b++) {
        if (fabsf(s.bands[b]) < 0.05f) continue;
        const char *kind = b == 0 ? "lowshelf" : b == MIC_BANDS - 1 ? "highshelf" : "equalizer";
        add("%s=f=%g:t=q:w=%g:g=%.1f", kind, MIC_BAND_HZ[b], MIC_BAND_Q[b], s.bands[b]);
    }
    if (fabsf(s.gain_db) >= 0.05f) add("volume=%.1fdB", s.gain_db);
    if (s.comp) add("%s", MIC_COMPRESS);
    if (!n) add("anull");
    return n;
}

// One RBJ biquad's gain in dB at `hz`: kind 0 = low shelf, 1 = peak, 2 = high shelf.
static f64 biquad_db(i32 kind, f64 f0, f64 q, f64 gain_db, f64 hz, f64 rate = 48000)
{
    const f64 pi = 3.14159265358979323846;
    f64 A = pow(10, gain_db / 40), w0 = 2 * pi * f0 / rate, cs = cos(w0), alpha = sin(w0) / (2 * q);
    f64 b0, b1, b2, a0, a1, a2;
    if (kind == 1) {
        b0 = 1 + alpha * A, b1 = -2 * cs, b2 = 1 - alpha * A;
        a0 = 1 + alpha / A, a1 = -2 * cs, a2 = 1 - alpha / A;
    } else {
        f64 sq = 2 * sqrt(A) * alpha, s = kind == 0 ? 1 : -1;
        b0 = A * ((A + 1) - s * (A - 1) * cs + sq);
        b1 = s * 2 * A * ((A - 1) - s * (A + 1) * cs);
        b2 = A * ((A + 1) - s * (A - 1) * cs - sq);
        a0 = (A + 1) + s * (A - 1) * cs + sq;
        a1 = -s * 2 * ((A - 1) + s * (A + 1) * cs);
        a2 = (A + 1) + s * (A - 1) * cs - sq;
    }
    // |H(e^jw)| for z = e^jw
    f64 w = 2 * pi * hz / rate, c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    f64 nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    f64 dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return 10 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

// What the EQ and gain do to a tone at `hz`, in dB (Clean and the compressor aside).
static f32 mic_curve_db(const MicSound &s, f32 hz)
{
    f64 db = s.gain_db;
    for (i32 b = 0; b < MIC_BANDS; b++)
        if (fabsf(s.bands[b]) >= 0.05f)
            db += biquad_db(b == 0 ? 0 : b == MIC_BANDS - 1 ? 2 : 1, MIC_BAND_HZ[b], MIC_BAND_Q[b], s.bands[b], hz);
    return (f32)db;
}
