// eftkills.cpp: Escape from Tarkov's kills, read off the kill list the game shows after a raid, and the markers its
// output log gives (automarks.cpp, eft_line) put into raids that are already over.
//
// The game logs no kill. After a raid it shows "RAID ENDED / YOUR KILL LIST", a row per kill: its time in the raid,
// counted from the player's spawn (the log's "PlayerSpawnEvent"), the name, level, faction and how ("Headshot (MPX,
// 22m)"). It is on screen between the raid's end (eft_line's end lines) and the game unloading SessionEndUIScene,
// often for a second or less, and only if the player opens it. Once that stretch is EK_SETTLE in the past, the
// footage Rekodr recorded of it is read at EK_FPS frames a second with ffmpeg. A frame whose title names the kill list
// is read with Windows' own OCR (Windows.Media.Ocr) in the language of the game's UI (Game.ini's "Language"); a
// language with no title text here, or no OCR pack on the PC, reads nothing. Only the kill list's own layout is
// trusted: its title, its column heads in their order, and rows that fit each column (ek_row_fits). Anything else on
// the screen, and every other end-of-raid screen, gives no kill. OCR can't read the time column's italic digital
// digits, so they are matched against EK_DIGITS. A row seen in several frames (the list scrolls) is kept once. Each
// becomes a Kill marker at spawn + its time, its note the weapon and the side killed ("MPX kill on Scav", PMC for USEC
// and BEAR, "Kill on PMC" with no weapon read), its detail the side and weapon. The player's name and level are never
// kept.
//
// Raids are found in every run's output log in <game>\Logs, so raids recorded before this ran get their kills too,
// and the log's other markers (raid start and end, quests, groups), each at its line's time, unless a marker with the
// same note is already within EK_SAME of it; that one only gets the log's colour and the keys of its detail it
// lacks. What has been done is kept in <local_dir>\eftkills.tsv. It runs on a
// thread of its own at low priority, started from the auto markers worker (eftkills_poll); only the game's logs and
// Rekodr's own buffer are read.

static const i64 EK_EVERY = 30000000;        // how often the logs are looked at
static const i64 EK_SETTLE = 60000000;       // how long after its kill list a raid is read
static const i64 EK_WINDOW_MAX = 180000000;  // the longest stretch read after a raid's end
static const i64 EK_GIVE_UP = 900000000;     // a raid this old is read with whatever footage there is
static const i64 EK_KEEP = 30ll * 86400 * 1000000;
static const i64 EK_SAME = 5000000;
static const i32 EK_FPS = 4;                 // the list may be up for only a second
enum { EK_W = 2560, EK_H = 1440 };           // every frame is scaled to this height and cropped or padded to this width

// ---------------------------------------------------------------------------------------------------------------
// The output log

struct EkRaid {
    i64  spawn_us, end_us, close_us; // close_us: SessionEndUIScene unloaded; 0 if the log doesn't say
    char map[32];
};

struct EkLogMark {
    i64  t_us;
    u32  event; // its AmEvent bit
    u8   colour;
    char note[64];
    char detail[256];
};

// A head line's "2026-11-27 03:37:00.499|", the PC's local time, on the wall clock. 0 if it isn't one.
static i64 ek_line_time(const char *line)
{
    int y, mo, d, h, mi, s, ms;
    if (sscanf(line, "%4d-%2d-%2d %2d:%2d:%2d.%3d|", &y, &mo, &d, &h, &mi, &s, &ms) != 7) return 0;
    SYSTEMTIME st = {(WORD)y, (WORD)mo, 0, (WORD)d, (WORD)h, (WORD)mi, (WORD)s, (WORD)ms};
    return replay_local_us(st);
}

// What one run's output log text holds: its raids, and the markers eft_line makes of it (of the AmEvent bits in
// `events`), each at the time of the head line it is in or follows. `seen` carries over from the run's earlier
// files. Adds to raids[*rn] and marks[*mn].
static void ek_log_read(char *text, u32 events, EftSeen *seen, EkRaid *raids, i32 rcap, i32 *rn, EkLogMark *marks, i32 mcap,
                        i32 *mn)
{
    i64 at = 0, spawn = 0;
    EkRaid *closing = 0; // the raid whose kill list may be up
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        if (i64 t = ek_line_time(line)) at = t;
        bool was = seen->in_raid;
        AutoMark am[4];
        i32 n = eft_line(seen, events, line, false, am, COUNT(am));
        for (i32 i = 0; i < n && at; i++)
            if (am[i].amend) { // the side of the last raid: its start's detail, its start's and end's colour
                char k[32], v[64], o[64];
                for (i32 j = *mn - 1; j >= 0; j--) {
                    if (marker_detail_get(marks[j].detail, "span_end", v, sizeof v)) marks[j].colour = am[i].colour;
                    if (!marker_detail_get(marks[j].detail, "span", v, sizeof v)) continue;
                    marks[j].colour = am[i].colour;
                    for (const char *d = am[i].detail; marker_detail_next(&d, k, sizeof k, v, sizeof v);)
                        if (!marker_detail_get(marks[j].detail, k, o, sizeof o)) marker_detail_add(marks[j].detail, k, v);
                    break;
                }
            } else if (*mn < mcap) {
                EkLogMark &m = marks[(*mn)++];
                m.t_us = at;
                m.event = am[i].event;
                m.colour = am[i].colour;
                str_set(m.note, am[i].note);
                str_set(m.detail, am[i].detail);
            }
        if (strstr(line, "|scene preset path:maps/")) spawn = 0, closing = 0;
        else if (strstr(line, "|PlayerSpawnEvent:")) spawn = at;
        else if (closing && strstr(line, "Unloading the last loaded scene") && strstr(line, "SessionEndUIScene")) {
            closing->close_us = at;
            closing = 0;
        }
        if (was && !seen->in_raid && at && *rn < rcap) {
            EkRaid &r = raids[(*rn)++];
            r = {spawn, at, 0, ""};
            str_set(r.map, seen->map);
            closing = &r;
        }
        if (!end) break;
        line = end + 1;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The time column: "00:02:53" in an italic digital font, each digit a 6x10 grid of how much of it is lit (0-100),
// from the kill lists of real raids. The text leans right by EK_SLANT; it is stood up before the digits are cut.

static const f32 EK_SLANT = 0.2f;
static const f32 EK_DIGIT_MAX = 8.0f; // the most a digit may differ from its grid; real ones differ by 6 at most
static const u8 EK_DIGITS[10][60] = {
    { 27, 100, 100, 100,  95,  19,  92,  34,  20,  21,  98,  75,
     100,   0,   0,  43,  93,  88,  96,   0,   0,  89,  34,  91,
      95,   4,  40,  77,   2,  98,  95,   5,  85,  29,   1,  98,
      93,  48,  81,   0,   1,  99,  91,  98,  34,   0,   0,  99,
      66,  96,  20,  20,  50,  86,   7,  85, 100, 100,  95,  19},
    {}, // a 1 is told by its width
    { 88, 100, 100, 100,  97,  21,  19,  20,  20,  20,  45,  77,
       0,   0,   0,   0,   8,  93,   0,   0,   0,   0,  19,  93,
       0,   0,   0,   9,  87,  44,   0,   0,  22,  89,  61,   1,
       1,  40,  95,  43,   0,   0,  52,  90,  27,   0,   0,   0,
      91,  40,  20,  20,  20,  20,  80, 100, 100, 100, 100, 100},
    { 93, 100,  99,  99, 100,  70,  13,  20,  19,  38, 100,  30,
       0,   0,  23,  92,  39,   0,   0,  13,  92,  50,   0,   0,
       0,  32, 100, 100,  89,  12,   0,   6,  20,  20,  52,  71,
       0,   0,   0,   0,   2,  87,   0,   0,   0,   0,   1,  96,
      44,  20,  20,  20,  58,  76,  46, 100, 100, 100,  88,  10},
    {  0,   0,   0,  30,  98,   6,   0,   0,   5,  84, 100,   6,
       0,   1,  63,  65,  89,   6,   0,  21,  90,   9,  88,   6,
       8,  84,  33,   0,  86,  10,  61,  66,   0,   0,  81,  13,
      98,  70,  60,  60,  95,  60,  58,  60,  60,  60,  95,  73,
       0,   0,   0,   0,  75,  19,   0,   0,   0,   0,  67,  14},
    {100, 100, 100, 100, 100,  76, 100,  25,  20,  20,  20,  13,
     100,   6,   0,   0,   0,   0, 100,  17,   0,   0,   0,   0,
      93, 100, 100, 100,  98,  25,  13,  20,  20,  20,  42,  90,
       0,   0,   0,   0,   6, 100,   0,   0,   0,   0,   6, 100,
      43,  22,  20,  20,  56,  89,  46,  98, 100, 100,  89,  21},
    { 27, 100, 100, 100, 100,  53,  80,  47,  20,  20,  20,  13,
     100,   0,   0,   0,   0,   0,  73,  13,   0,   0,   0,   0,
      67, 100, 100, 100, 100,  27,  67,  47,  20,  20,  33,  93,
      67,  33,   0,   0,   0, 100,  67,  33,   0,   0,   0, 100,
      60,  60,  20,  20,  33,  93,   7,  73, 100, 100, 100,  27},
    { 93, 100,  99,  99, 100,  70,   0,   0,   0,   0,  90,  90,
       0,   0,   0,   0,  90,  90,   0,   0,   0,   0,  90,  90,
       0,   0,   0,   0,  90,  90,   0,   0,   0,   0,  90,  90,
       0,   0,   0,   0,  90,  90,   0,   0,   0,   0,  90,  90,
       0,   0,   0,   0,  90,  90,   0,   0,   0,   0,  90,  90},
    { 29,  99, 100, 100,  92,  16,  97,  36,  20,  20,  49,  76,
     100,   3,   0,   0,  20,  83, 100,  17,   0,   0,  33,  83,
      45, 100, 100, 100, 100,  42,  73,  64,  20,  20,  63,  76,
     100,   3,   0,   0,   3, 100, 100,   3,   0,   0,   3, 100,
      83,  56,  20,  20,  49,  83,  10,  84, 100, 100,  92,  16},
    { 27, 100, 100, 100, 100,  27,  93,  33,  20,  20,  33,  80,
     100,   0,   0,   0,   0, 100, 100,   0,   0,   0,   0, 100,
      80,  33,   0,   0,   7, 100,  27, 100,  80,  80,  87, 100,
       0,   0,   0,   0,  13, 100,   0,   0,   0,   0,   0, 100,
      27,  33,  20,  20,  33,  93,  40,  87, 100, 100, 100,  27},
};

struct EkBox { i32 x0, x1, y0, y1, area; };

// The seconds a time cell shows (px: its grey pixels, w by h, `stride` apart), -1 if it can't be read.
static i32 ek_time_secs(const u8 *px, i32 stride, i32 w, i32 h)
{
    enum { MAX_W = 320, MAX_H = 64 };
    if (w < 8 || h < 8 || h > MAX_H || w + (i32)(EK_SLANT * h) + 2 > MAX_W) return -1;
    static thread_local f32 o[MAX_H][MAX_W];
    static thread_local u8 lit[MAX_H][MAX_W];
    static thread_local i32 stack[MAX_H * MAX_W];
    i32 k = (i32)(EK_SLANT * h), ow = w + k + 2;
    f32 top = 0;
    for (i32 y = 0; y < h; y++)
        for (i32 x = 0; x < ow; x++) {
            f32 p = x + EK_SLANT * (h - 1 - y) - k, v = 0;
            if (p >= 0 && p <= w - 1) {
                i32 i = (i32)p, j = i + 1 < w ? i + 1 : w - 1;
                f32 f = p - i;
                v = px[y * stride + i] * (1 - f) + px[y * stride + j] * f;
            }
            o[y][x] = v;
            if (v > top) top = v;
        }
    if (top < 40) return -1;
    f32 th = top * 0.45f;
    for (i32 y = 0; y < h; y++)
        for (i32 x = 0; x < ow; x++) lit[y][x] = o[y][x] > th;
    // The lit parts, 8-connected, left to right; a part over another (the colon's dots) is one glyph with it.
    EkBox box[16];
    i32 nb = 0;
    for (i32 x = 0; x < ow; x++)
        for (i32 y = 0; y < h; y++) {
            if (lit[y][x] != 1) continue;
            EkBox b = {x, x, y, y, 0};
            i32 sp = 0;
            stack[sp++] = y * MAX_W + x;
            lit[y][x] = 2;
            while (sp) {
                i32 at = stack[--sp], cy = at / MAX_W, cx = at % MAX_W;
                b.area++;
                if (cx < b.x0) b.x0 = cx;
                if (cx > b.x1) b.x1 = cx;
                if (cy < b.y0) b.y0 = cy;
                if (cy > b.y1) b.y1 = cy;
                for (i32 dy = -1; dy <= 1; dy++)
                    for (i32 dx = -1; dx <= 1; dx++) {
                        i32 ny = cy + dy, nx = cx + dx;
                        if (ny < 0 || ny >= h || nx < 0 || nx >= ow || lit[ny][nx] != 1) continue;
                        lit[ny][nx] = 2;
                        stack[sp++] = ny * MAX_W + nx;
                    }
            }
            if (b.area < 3) continue;
            b.x1++, b.y1++;
            if (nb) {
                EkBox &p = box[nb - 1];
                i32 over = (b.x1 < p.x1 ? b.x1 : p.x1) - (b.x0 > p.x0 ? b.x0 : p.x0);
                i32 narrow = b.x1 - b.x0 < p.x1 - p.x0 ? b.x1 - b.x0 : p.x1 - p.x0;
                if (over * 2 > narrow) {
                    p = {p.x0 < b.x0 ? p.x0 : b.x0, p.x1 > b.x1 ? p.x1 : b.x1, p.y0 < b.y0 ? p.y0 : b.y0,
                         p.y1 > b.y1 ? p.y1 : b.y1, p.area + b.area};
                    continue;
                }
            }
            if (nb == COUNT(box)) return -1;
            box[nb++] = b;
        }
    if (nb != 8) return -1;
    i32 tall = 0;
    for (i32 i = 0; i < nb; i++)
        if (box[i].y1 - box[i].y0 > tall) tall = box[i].y1 - box[i].y0;
    i32 digit[8];
    for (i32 i = 0; i < 8; i++) {
        const EkBox &b = box[i];
        i32 bw = b.x1 - b.x0, bh = b.y1 - b.y0;
        bool colon = bh < 0.8f * tall;
        if (colon != (i == 2 || i == 5)) return -1;
        if (colon) continue;
        if (bw < 0.6f * tall) {
            digit[i] = 1;
            continue;
        }
        // How much of each of the 6x10 cells is lit, a pixel counting for the part of it in the cell.
        f32 grid[60];
        for (i32 gy = 0; gy < 10; gy++) {
            f32 ya = b.y0 + gy * bh / 10.0f, yb = b.y0 + (gy + 1) * bh / 10.0f;
            for (i32 gx = 0; gx < 6; gx++) {
                f32 xa = b.x0 + gx * bw / 6.0f, xb = b.x0 + (gx + 1) * bw / 6.0f, sum = 0, area = 0;
                for (i32 y = (i32)ya; y < b.y1 && y < yb; y++) {
                    f32 wy = fminf(yb, y + 1.0f) - fmaxf(ya, (f32)y);
                    if (wy <= 0) continue;
                    for (i32 x = (i32)xa; x < b.x1 && x < xb; x++) {
                        f32 wx = fminf(xb, x + 1.0f) - fmaxf(xa, (f32)x);
                        if (wx <= 0) continue;
                        sum += wx * wy * (lit[y][x] ? 1 : 0);
                        area += wx * wy;
                    }
                }
                grid[gy * 6 + gx] = area > 0 ? sum / area : 0;
            }
        }
        f32 best = 1e9f;
        for (i32 d = 0; d < 10; d++) {
            if (d == 1) continue;
            f32 diff = 0;
            for (i32 c = 0; c < 60; c++) diff += fabsf(grid[c] - EK_DIGITS[d][c] / 100.0f);
            if (diff < best) best = diff, digit[i] = d;
        }
        if (best > EK_DIGIT_MAX) return -1;
    }
    if (digit[3] > 5 || digit[6] > 5) return -1;
    return (digit[0] * 10 + digit[1]) * 3600 + (digit[3] * 10 + digit[4]) * 60 + digit[6] * 10 + digit[7];
}

// ---------------------------------------------------------------------------------------------------------------
// Windows' OCR, through its WinRT interfaces (combase.dll's, looked up when first used)

struct EkWord {
    char text[64]; // UTF-8
    f32  x, y, w, h;
};

typedef HRESULT(WINAPI *EkRoFactory)(HSTRING, REFIID, void **);
typedef HRESULT(WINAPI *EkCreateString)(PCNZWCH, UINT32, HSTRING *);
typedef HRESULT(WINAPI *EkDeleteString)(HSTRING);
typedef PCWSTR(WINAPI *EkStringBuffer)(HSTRING, UINT32 *);

static struct {
    SRWLOCK lock;
    bool tried;
    EkRoFactory factory;
    EkCreateString create;
    EkDeleteString destroy;
    EkStringBuffer buffer;
    ABI::Windows::Media::Ocr::IOcrEngineStatics *ocr;
    ABI::Windows::Globalization::ILanguageFactory *lang;
    ABI::Windows::Graphics::Imaging::ISoftwareBitmapFactory *bitmaps;
    ABI::Windows::Media::Ocr::IOcrEngine *engine;
    wchar_t engine_tag[16];
} g_ocr;

template <class T> static bool ek_factory(const wchar_t *cls, T **out)
{
    HSTRING h;
    if (FAILED(g_ocr.create(cls, (UINT32)wcslen(cls), &h))) return false;
    HRESULT hr = g_ocr.factory(h, __uuidof(T), (void **)out);
    g_ocr.destroy(h);
    return SUCCEEDED(hr);
}

// An OCR engine for a language ("en-US"); false if the PC has none for it. The thread must have COM started.
static bool ek_ocr_open(const wchar_t *tag)
{
    AcquireSRWLockExclusive(&g_ocr.lock);
    if (!g_ocr.tried) {
        g_ocr.tried = true;
        HMODULE cb = LoadLibraryW(L"combase.dll");
        g_ocr.factory = cb ? (EkRoFactory)(void *)GetProcAddress(cb, "RoGetActivationFactory") : 0;
        g_ocr.create = cb ? (EkCreateString)(void *)GetProcAddress(cb, "WindowsCreateString") : 0;
        g_ocr.destroy = cb ? (EkDeleteString)(void *)GetProcAddress(cb, "WindowsDeleteString") : 0;
        g_ocr.buffer = cb ? (EkStringBuffer)(void *)GetProcAddress(cb, "WindowsGetStringRawBuffer") : 0;
        if (g_ocr.factory && g_ocr.create && g_ocr.destroy && g_ocr.buffer &&
            !(ek_factory(L"Windows.Media.Ocr.OcrEngine", &g_ocr.ocr) && ek_factory(L"Windows.Globalization.Language", &g_ocr.lang) &&
              ek_factory(L"Windows.Graphics.Imaging.SoftwareBitmap", &g_ocr.bitmaps)))
            log_line("markers: Escape from Tarkov: Windows OCR isn't there");
    }
    if (g_ocr.engine && wcscmp(g_ocr.engine_tag, tag)) {
        g_ocr.engine->Release();
        g_ocr.engine = 0;
    }
    if (!g_ocr.engine && g_ocr.ocr && g_ocr.lang && g_ocr.bitmaps) {
        HSTRING h;
        ABI::Windows::Globalization::ILanguage *lang = 0;
        if (SUCCEEDED(g_ocr.create(tag, (UINT32)wcslen(tag), &h))) {
            g_ocr.lang->CreateLanguage(h, &lang);
            g_ocr.destroy(h);
        }
        boolean ok = 0;
        if (lang && SUCCEEDED(g_ocr.ocr->IsLanguageSupported(lang, &ok)) && ok) g_ocr.ocr->TryCreateFromLanguage(lang, &g_ocr.engine);
        if (lang) lang->Release();
        if (g_ocr.engine) wcscpy_s(g_ocr.engine_tag, tag);
    }
    bool ok = g_ocr.engine != 0;
    ReleaseSRWLockExclusive(&g_ocr.lock);
    return ok;
}

// The words OCR reads in a w by h part of a grey picture (`stride` apart), in the engine ek_ocr_open made; their
// places are in that part. How many; -1 if it couldn't read.
static i32 ek_ocr(const u8 *px, i32 stride, i32 w, i32 h, EkWord *out, i32 cap)
{
    namespace Img = ABI::Windows::Graphics::Imaging;
    namespace Fnd = ABI::Windows::Foundation;
    namespace Ocr = ABI::Windows::Media::Ocr;
    AcquireSRWLockExclusive(&g_ocr.lock);
    i32 n = -1;
    Img::ISoftwareBitmap *bmp = 0;
    Img::IBitmapBuffer *bb = 0;
    Fnd::IMemoryBuffer *mb = 0;
    Fnd::IMemoryBufferReference *ref = 0;
    ::Windows::Foundation::IMemoryBufferByteAccess *bytes = 0;
    Fnd::IClosable *close = 0;
    Fnd::IAsyncOperation<Ocr::OcrResult *> *op = 0;
    Fnd::IAsyncInfo *info = 0;
    Ocr::IOcrResult *res = 0;
    Fnd::Collections::IVectorView<Ocr::OcrLine *> *lines = 0;
    BYTE *dst = 0;
    UINT32 dst_len = 0;
    Img::BitmapPlaneDescription plane = {};
    AsyncStatus st = AsyncStatus::Started;
    unsigned count = 0;
    u64 deadline = GetTickCount64() + 10000;
    if (!g_ocr.engine || FAILED(g_ocr.bitmaps->Create(Img::BitmapPixelFormat_Gray8, w, h, &bmp))) goto done;
    if (FAILED(bmp->LockBuffer(Img::BitmapBufferAccessMode_Write, &bb)) || FAILED(bb->GetPlaneDescription(0, &plane)) ||
        FAILED(bb->QueryInterface(__uuidof(Fnd::IMemoryBuffer), (void **)&mb)) || FAILED(mb->CreateReference(&ref)) ||
        FAILED(ref->QueryInterface(__uuidof(::Windows::Foundation::IMemoryBufferByteAccess), (void **)&bytes)) ||
        FAILED(bytes->GetBuffer(&dst, &dst_len)) || (u64)plane.StartIndex + (u64)plane.Stride * (h - 1) + w > dst_len)
        goto done;
    for (i32 y = 0; y < h; y++) memcpy(dst + plane.StartIndex + (size_t)y * plane.Stride, px + (size_t)y * stride, (size_t)w);
    bytes->Release(), bytes = 0;
    ref->Release(), ref = 0;
    if (SUCCEEDED(bb->QueryInterface(__uuidof(Fnd::IClosable), (void **)&close))) close->Close();
    if (FAILED(g_ocr.engine->RecognizeAsync(bmp, &op)) || FAILED(op->QueryInterface(__uuidof(Fnd::IAsyncInfo), (void **)&info)))
        goto done;
    while (SUCCEEDED(info->get_Status(&st)) && st == AsyncStatus::Started && GetTickCount64() < deadline) Sleep(2);
    if (st != AsyncStatus::Completed || FAILED(op->GetResults(&res)) || FAILED(res->get_Lines(&lines))) {
        if (st == AsyncStatus::Started) info->Cancel();
        goto done;
    }
    lines->get_Size(&count);
    n = 0;
    for (unsigned i = 0; i < count; i++) {
        Ocr::IOcrLine *line = 0;
        Fnd::Collections::IVectorView<Ocr::OcrWord *> *words = 0;
        unsigned k = 0;
        if (FAILED(lines->GetAt(i, &line))) continue;
        if (SUCCEEDED(line->get_Words(&words))) words->get_Size(&k);
        for (unsigned j = 0; j < k; j++) {
            Ocr::IOcrWord *word = 0;
            HSTRING text = 0;
            Fnd::Rect r = {};
            if (FAILED(words->GetAt(j, &word))) continue;
            if (n < cap && SUCCEEDED(word->get_Text(&text)) && SUCCEEDED(word->get_BoundingRect(&r))) {
                EkWord &o = out[n++];
                wide_to_utf8(g_ocr.buffer(text, 0), o.text, sizeof o.text);
                o.x = r.X, o.y = r.Y, o.w = r.Width, o.h = r.Height;
            }
            if (text) g_ocr.destroy(text);
            word->Release();
        }
        if (words) words->Release();
        line->Release();
    }
done:
    if (lines) lines->Release();
    if (res) res->Release();
    if (info) info->Release();
    if (op) op->Release();
    if (close) close->Release();
    if (bytes) bytes->Release();
    if (ref) ref->Release();
    if (mb) mb->Release();
    if (bb) bb->Release();
    if (bmp) bmp->Release();
    ReleaseSRWLockExclusive(&g_ocr.lock);
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// The kill list

// What the kill list says in a language of the game's UI (Game.ini's "Language"), upper case, and the OCR
// languages that read it, best first.
struct EkLang {
    const char *game;
    const wchar_t *ocr[2];
    const char *ended, *title;                                       // the screen's head and the line under it
    const char *location, *time, *player, *level, *faction, *status; // the column heads, left to right
    const char *factions[3];                                         // what the faction column may say, the Scavs' last
};
static const EkLang EK_LANGS[] = {
    {"en", {L"en-US", L"en-GB"}, "RAID ENDED", "KILL LIST", "LOCATION", "TIME", "PLAYER", "LVL", "FACTION", "STATUS",
     {"USEC", "BEAR", "SCAV"}},
};

struct EkKill {
    i32  secs;    // into the raid, from the player's spawn
    char side[8]; // PMC or Scav
    char weapon[48];
};

// Words in [x0, x1) of a row, left to right, as one text of printable ASCII: words OCR split with almost no gap
// between them (a name's "_1" read apart) are put back together.
static void ek_cell_text(const EkWord *words, const i32 *row, i32 n, f32 x0, f32 x1, char *out, i32 cap)
{
    i32 len = 0;
    f32 right = -1;
    out[0] = 0;
    for (i32 i = 0; i < n; i++) {
        const EkWord &w = words[row[i]];
        if (w.x < x0 || w.x >= x1) continue;
        char text[64];
        i32 tl = 0;
        for (const char *c = w.text; *c && tl < (i32)sizeof text - 1; c++)
            if (*c >= 0x20 && *c < 0x7f) text[tl++] = *c;
        text[tl] = 0;
        if (!tl) continue;
        bool glued = right >= 0 && w.x - right < w.h * 0.3f;
        len += snprintf(out + len, len < cap ? cap - len : 0, "%s%s", len && !glued ? " " : "", text);
        if (len >= cap) len = cap - 1;
        right = w.x + w.w;
    }
}

static bool ek_word_is(const EkWord &w, const char *text) { return !_stricmp(w.text, text); }

// Whether a row's cells fit the kill list's columns: a location with a letter in it; a level of "--" (or nothing, the
// dashes not read) or 1 to 99; one of the language's factions; and a status of "<how> (<weapon>, ..., <distance>m)",
// "Headshot (MPX, 22m)" or "Killed (MPX, thorax, 16.8m)".
static bool ek_row_fits(const EkLang &lang, const char *location, const char *level, const char *faction, const char *how)
{
    bool letter = false;
    for (const char *c = location; *c; c++) letter |= isalpha((u8)*c) != 0;
    if (!letter) return false;
    i32 digits = 0, dashes = 0, other = 0;
    for (const char *c = level; *c; c++) {
        if (isdigit((u8)*c)) digits++;
        else if (*c == '-' || *c == '_' || *c == '~') dashes++;
        else if (*c != ' ') other++;
    }
    if (other || (digits && dashes) || digits > 2 || (digits && atoi(level) < 1)) return false;
    bool known = false;
    for (const char *f : lang.factions) known |= f && !_stricmp(faction, f);
    if (!known) return false;
    const char *open = strchr(how, '(');
    if (!open || open == how) return false;
    for (const char *c = how; c < open; c++)
        if (!isalpha((u8)*c) && *c != ' ') return false;
    size_t len = strlen(how);
    const char *end = how + len; // past the ')'
    if (len < 4 || end[-1] != ')') return false;
    const char *unit = end - 2;
    if (*unit == 'n' && unit[-1] == 'r') unit--; // OCR reads "m" as "rn"
    else if (*unit != 'm') return false;
    const char *num = unit;
    while (num > open && (isdigit((u8)num[-1]) || num[-1] == '.')) num--;
    if (num == unit || !isdigit((u8)*num)) return false;
    const char *comma = strchr(open, ',');
    return comma && comma < num && comma > open + 1;
}

// The rows of the kill list in a frame of EK_W by EK_H grey pixels; -1 if the frame isn't the kill list.
static i32 ek_kill_list(const u8 *px, const EkLang &lang, EkKill *out, i32 cap)
{
    static thread_local EkWord words[512];
    i32 tx = EK_W * 35 / 100, tw = EK_W * 30 / 100, th = EK_H * 9 / 100;
    i32 n = ek_ocr(px + tx, EK_W, tw, th, words, COUNT(words));
    char title[256] = "";
    for (i32 i = 0, len = 0; i < n && len < (i32)sizeof title - 1; i++)
        len += snprintf(title + len, sizeof title - len, " %s", words[i].text);
    _strupr_s(title);
    if (!strstr(title, lang.ended) || !strstr(title, lang.title)) return -1;
    n = ek_ocr(px, EK_W, EK_W, EK_H, words, COUNT(words));
    const EkWord *location = 0, *time = 0, *player = 0, *level = 0, *faction = 0, *status = 0;
    for (i32 i = 0; i < n; i++) {
        const EkWord &w = words[i];
        if (ek_word_is(w, lang.time)) time = &w;
    }
    if (!time) return 0;
    for (i32 i = 0; i < n; i++) {
        const EkWord &w = words[i];
        if (fabsf(w.y - time->y) > w.h) continue;
        if (ek_word_is(w, lang.location)) location = &w;
        else if (ek_word_is(w, lang.player)) player = &w;
        else if (ek_word_is(w, lang.level)) level = &w;
        else if (ek_word_is(w, lang.faction)) faction = &w;
        else if (ek_word_is(w, lang.status)) status = &w;
    }
    if (!location || !player || !level || !faction || !status ||
        !(location->x < time->x && time->x < player->x && player->x < level->x && level->x < faction->x &&
          faction->x < status->x))
        return 0;
    f32 pad = time->h, head_y = time->y;
    // A row is where the player column has words, from the top, each with the words beside it.
    bool used[COUNT(words)] = {};
    i32 rows = 0;
    for (;;) {
        i32 first = -1;
        for (i32 i = 0; i < n; i++) {
            const EkWord &w = words[i];
            if (used[i] || w.y < head_y + 2 * pad || w.x < player->x - pad || w.x >= level->x - pad) continue;
            if (first < 0 || w.y < words[first].y) first = i;
        }
        if (first < 0) break;
        f32 mid = words[first].y + words[first].h / 2;
        i32 row[64], k = 0;
        for (i32 i = 0; i < n && k < COUNT(row); i++)
            if (!used[i] && fabsf(words[i].y + words[i].h / 2 - mid) < pad) row[k++] = i;
        for (i32 i = 0; i < k; i++) used[row[i]] = true;
        for (i32 i = 1; i < k; i++) // left to right
            for (i32 j = i; j > 0 && words[row[j]].x < words[row[j - 1]].x; j--) {
                i32 t = row[j];
                row[j] = row[j - 1], row[j - 1] = t;
            }
        EkKill kill = {};
        char where[64], who[64], lv[16], side[24], how[128];
        ek_cell_text(words, row, k, location->x - pad, time->x - pad, where, sizeof where);
        ek_cell_text(words, row, k, player->x - pad, level->x - pad, who, sizeof who);
        ek_cell_text(words, row, k, level->x - pad, faction->x - pad, lv, sizeof lv);
        ek_cell_text(words, row, k, faction->x - pad, status->x - pad, side, sizeof side);
        ek_cell_text(words, row, k, status->x - pad, 1e9f, how, sizeof how);
        if (!who[0] || !ek_row_fits(lang, where, lv, side, how)) continue;
        str_set(kill.side, !_stricmp(side, lang.factions[COUNT(lang.factions) - 1]) ? "Scav" : "PMC");
        // "Headshot (MPX, 22m)": the weapon is what the brackets start with.
        if (const char *open = strchr(how, '(')) {
            open++;
            size_t l = strcspn(open, ",)");
            while (l && open[l - 1] == ' ') l--;
            if (l >= sizeof kill.weapon) l = sizeof kill.weapon - 1;
            memcpy(kill.weapon, open, l);
            kill.weapon[l] = 0;
        }
        // The time cell: from the column's head to the player column, the height of the row's text and a little.
        i32 x0 = (i32)(time->x - pad * 0.7f), x1 = (i32)(player->x - pad * 1.4f);
        i32 y0 = (i32)(words[first].y - pad * 0.55f), y1 = (i32)(words[first].y + pad * 2.1f);
        if (x0 < 0 || y0 < 0 || x1 > EK_W || y1 > EK_H || x1 <= x0 || y1 <= y0) continue;
        kill.secs = ek_time_secs(px + (size_t)y0 * EK_W + x0, EK_W, x1 - x0, y1 - y0);
        if (kill.secs < 0 || rows == cap) continue;
        out[rows++] = kill;
    }
    return rows;
}

// A kill's marker note: "MPX kill on Scav", or "Kill on PMC" with no weapon
static void ek_kill_note(const EkKill &k, char *out, i32 cap)
{
    if (k.weapon[0]) snprintf(out, cap, "%s kill on %s", k.weapon, k.side);
    else snprintf(out, cap, "Kill on %s", k.side);
}

// Adds the rows of a frame to the kills read so far. Rows of the same second and side are the same kills: a frame
// adds only those beyond the ones already there (two kills in one second are two rows of one frame), and a weapon
// one read missed is taken from another.
static void ek_merge(EkKill *all, i32 cap, i32 *n, const EkKill *rows, i32 k)
{
    auto same = [](const EkKill &a, const EkKill &b) { return a.secs == b.secs && !strcmp(a.side, b.side); };
    i32 had = *n;
    for (i32 i = 0; i < k; i++) {
        const EkKill &r = rows[i];
        i32 before = 0, seen = 0, at = -1;
        for (i32 j = 0; j < i; j++) before += same(rows[j], r);
        for (i32 j = 0; j < had; j++)
            if (same(all[j], r) && seen++ == before) at = j;
        if (at < 0) {
            if (*n < cap) all[(*n)++] = r;
        } else if (!all[at].weapon[0]) str_set(all[at].weapon, r.weapon);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The footage

// Reads the kill list from the buffer's footage of [from, to), EK_FPS frames a second, into kills. How many; -1 if
// the footage of it isn't all in the buffer yet and it's worth waiting for.
static i32 ek_footage_kills(const Path &buffer_dir, const Path &ffmpeg, const EkLang &lang, i64 from, i64 to, bool wait,
                            EkKill *kills, i32 cap)
{
    static BufferIdx *b;
    if (!b) b = (BufferIdx *)VirtualAlloc(0, sizeof(BufferIdx), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!b || !buffer_read(buffer_dir, b)) return wait ? -1 : 0;
    if (wait && b->newest_us < to && b->video_written_us > now_us() - EK_SETTLE) return -1;
    static u8 *frame;
    if (!frame) frame = (u8 *)malloc((size_t)EK_W * EK_H);
    if (!frame) return 0;
    i32 n = 0, frames = 0, lists = 0;
    for (i32 s = 0; s < b->seg_count; s++) {
        const VideoSeg &v = b->segs[s];
        if (v.end_us <= from || v.start_us >= to) continue;
        i64 a = from > v.start_us ? from : v.start_us, z = to < v.end_us ? to : v.end_us;
        wchar_t run[64], name[32], file[MAX_PATH];
        utf8_to_wide(b->runs[v.run].name, run, COUNT(run));
        utf8_to_wide(v.name, name, COUNT(name));
        swprintf(file, MAX_PATH, L"%s\\video\\%s\\%s", buffer_dir.s, run, name);
        wchar_t args[4096];
        args[0] = 0;
        i32 ac = COUNT(args);
        static const char *const head[] = {"-hide_banner", "-loglevel", "error", "-nostdin", "-threads", "2"};
        arg_list(args, ac, head, COUNT(head));
        arg_pair(args, ac, "-ss", "%.3f", (f64)(a - v.start_us) / 1e6);
        arg_pair(args, ac, "-t", "%.3f", (f64)(z - a) / 1e6);
        cmd_arg8(args, ac, "-i");
        cmd_arg(args, ac, file);
        arg_pair(args, ac, "-vf", "fps=%d,scale=-2:%d,crop='min(iw,%d)':%d,pad=%d:%d:-1:-1,format=gray", EK_FPS, EK_H, EK_W,
                 EK_H, EK_W, EK_H);
        static const char *const tail[] = {"-an", "-sn", "-f", "rawvideo", "pipe:1"};
        arg_list(args, ac, tail, COUNT(tail));
        Proc p;
        if (!proc_start(ffmpeg.s, args, PROC_STDOUT, &p)) continue;
        SetPriorityClass(p.process, IDLE_PRIORITY_CLASS);
        size_t got = 0;
        u64 deadline = GetTickCount64() + 30000; // moved on by each frame
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(p.out, 0, 0, 0, &avail, 0)) break;
            if (!avail) {
                if (GetTickCount64() > deadline) {
                    proc_kill(&p);
                    break;
                }
                Sleep(5);
                continue;
            }
            DWORD want = (DWORD)((size_t)EK_W * EK_H - got), read = 0;
            if (want > avail) want = avail;
            if (!ReadFile(p.out, frame + got, want, &read, 0) || !read) break;
            got += read;
            if (got < (size_t)EK_W * EK_H) continue;
            got = 0;
            frames++;
            deadline = GetTickCount64() + 30000;
            EkKill rows[32];
            i32 k = ek_kill_list(frame, lang, rows, COUNT(rows));
            if (k >= 0) lists++;
            if (k > 0) ek_merge(kills, cap, &n, rows, k);
        }
        if (proc_wait(&p, 2000) < 0) proc_kill(&p);
        proc_close(&p);
    }
    log_line("markers: Escape from Tarkov: %d frames read, %d of the kill list, %d kills", frames, lists, n);
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// The pass

static struct {
    SRWLOCK lock;
    HANDLE thread;
    i64 next;
    // Set by eftkills_apply on the main thread; the thread takes a copy as it starts.
    wchar_t eft_dir[MAX_PATH];
    Path buffer_dir, ffmpeg, local_dir;
    char lang[16];
} g_ek;

struct EkJob {
    wchar_t eft_dir[MAX_PATH];
    Path buffer_dir, ffmpeg, local_dir;
    char lang[16];
    u32 events;
};

// What has been done: the newest log marker handled, and the raids (by their spawn) whose kill list was read.
struct EkDone {
    i64 marks_us;
    i64 raids[512];
    i32 raid_count;
};

static void ek_done_load(const Path &local_dir, EkDone *d)
{
    *d = {};
    char *text = read_file(path_join(local_dir, L"eftkills.tsv").s, 0);
    if (!text) return;
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        long long t;
        if (sscanf(line, "marks\t%lld", &t) == 1) d->marks_us = t;
        else if (sscanf(line, "raid\t%lld", &t) == 1 && d->raid_count < COUNT(d->raids)) d->raids[d->raid_count++] = t;
        if (!end) break;
        line = end + 1;
    }
    free(text);
}

static void ek_done_write(const Path &local_dir, EkDone *d)
{
    i64 old = now_us() - EK_KEEP;
    FILE *f = _wfopen(path_join(local_dir, L"eftkills.tsv").s, L"wb");
    if (!f) return;
    fprintf(f, "marks\t%lld\n", d->marks_us);
    for (i32 i = 0; i < d->raid_count; i++)
        if (d->raids[i] >= old) fprintf(f, "raid\t%lld\n", d->raids[i]);
    fclose(f);
}

static bool ek_done_has(const EkDone &d, i64 spawn)
{
    for (i32 i = 0; i < d.raid_count; i++)
        if (d.raids[i] == spawn) return true;
    return false;
}

// The marker like this one that is there already, if any: the same note (up to a bracket) within `within` of t,
// and the same detail when `detail` is given.
static const Marker *ek_marked(i64 t, i64 within, const char *note, const char *detail)
{
    static Marker by[64];
    i32 n = markers_in(t - within, t + within, by, COUNT(by));
    size_t l = strcspn(note, "(");
    for (i32 i = 0; i < n; i++) {
        if (strncmp(by[i].note, note, l) || strcspn(by[i].note, "(") != l) continue;
        if (!detail || !strcmp(by[i].detail, detail)) return &by[i];
    }
    return 0;
}

// A log mark that is on the timeline already gives that marker its colour (a Scav raid's) and the keys of its detail
// the marker lacks (the band's span and span_end, the side). How many changes it made.
static i32 ek_fill(const Marker &have, const EkLogMark &m)
{
    char k[32], v[64], o[64];
    i32 n = 0;
    for (const char *at = m.detail; marker_detail_next(&at, k, sizeof k, v, sizeof v);)
        if (!marker_detail_get(have.detail, k, o, sizeof o)) markers_detail_put(have.t_us, k, v), n++;
    if (have.colour != m.colour) markers_colour_set(have.t_us, m.colour), n++;
    return n;
}

static void ek_post(i64 t, u32 event, u8 colour, const char *note, const char *detail)
{
    Event e = {};
    e.kind = EV_MARKER;
    e.a = t;
    e.b = colour;
    char d[256];
    marker_detail_event(d, event, detail);
    snprintf(e.text, sizeof e.text, "%s\t%s", note, d);
    event_post(e);
}

// Each run's output logs in <dir>\Logs, oldest first, read for raids and markers.
static void ek_logs(const wchar_t *dir, u32 events, EkRaid *raids, i32 rcap, i32 *rn, EkLogMark *marks, i32 mcap, i32 *mn)
{
    wchar_t pat[MAX_PATH], runs[64][MAX_PATH];
    i64 made[64];
    i32 nr = 0;
    swprintf(pat, MAX_PATH, L"%s\\Logs\\log_*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || filetime_to_us(fd.ftLastWriteTime) < now_us() - EK_KEEP) continue;
        i64 t = filetime_to_us(fd.ftCreationTime);
        i32 at = nr < COUNT(runs) ? nr++ : -1;
        if (at < 0) continue;
        while (at && made[at - 1] > t) { // oldest first
            made[at] = made[at - 1];
            wcscpy_s(runs[at], runs[at - 1]);
            at--;
        }
        made[at] = t;
        wcscpy_s(runs[at], fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (i32 r = 0; r < nr; r++) {
        // A run's output logs, "..._000.log" on, in order.
        wchar_t files[16][MAX_PATH];
        i32 nf = 0;
        swprintf(pat, MAX_PATH, L"%s\\Logs\\%s\\*.log", dir, runs[r]);
        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            char name[MAX_PATH];
            wide_to_utf8(fd.cFileName, name, sizeof name);
            if (eft_log_kind(name) != 0 || nf == COUNT(files)) continue;
            i32 at = nf++;
            while (at && _wcsicmp(files[at - 1], fd.cFileName) > 0) wcscpy_s(files[at], files[at - 1]), at--;
            wcscpy_s(files[at], fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        EftSeen seen = {};
        for (i32 f = 0; f < nf; f++) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%s\\Logs\\%s\\%s", dir, runs[r], files[f]);
            char *text = read_file(path, 0);
            if (!text) continue;
            ek_log_read(text, events, &seen, raids, rcap, rn, marks, mcap, mn);
            free(text);
        }
    }
}

static DWORD WINAPI ek_thread(void *arg)
{
    EkJob *j = (EkJob *)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    CoInitializeEx(0, COINIT_MULTITHREADED);
    static EkRaid raids[256];
    static EkLogMark marks[2048];
    static EkDone done;
    i32 nr = 0, nm = 0;
    ek_logs(j->eft_dir, j->events, raids, COUNT(raids), &nr, marks, COUNT(marks), &nm);
    ek_done_load(j->local_dir, &done);
    i64 now = now_us();
    bool changed = false;
    i32 put = 0, keyed = 0;
    for (i32 i = 0; i < nm; i++) {
        const EkLogMark &m = marks[i];
        if (m.t_us <= done.marks_us || m.t_us > now - EK_SETTLE) continue;
        if (const Marker *have = ek_marked(m.t_us, EK_SAME, m.note, 0)) keyed += ek_fill(*have, m);
        else ek_post(m.t_us, m.event, m.colour, m.note, m.detail), put++;
        done.marks_us = m.t_us;
        changed = true;
    }
    if (put) log_line("markers: Escape from Tarkov: %d from its logs, after the fact", put);
    if (keyed) log_line("markers: Escape from Tarkov: %d changes to markers already there", keyed);
    const EkLang *lang = 0;
    for (const EkLang &l : EK_LANGS)
        if (!strcmp(l.game, j->lang)) lang = &l;
    bool ocr = false;
    if (lang)
        for (const wchar_t *tag : lang->ocr)
            if (!ocr && tag) ocr = ek_ocr_open(tag);
    for (i32 i = 0; i < nr && (j->events & AM_KILL) && ocr && j->ffmpeg.s[0]; i++) {
        const EkRaid &r = raids[i];
        i64 to = r.close_us && r.close_us - r.end_us <= EK_WINDOW_MAX ? r.close_us : r.end_us + EK_WINDOW_MAX;
        if (!r.spawn_us || ek_done_has(done, r.spawn_us) || to > now - EK_SETTLE || r.end_us < now - EK_KEEP) continue;
        static EkKill kills[128];
        i32 n = ek_footage_kills(j->buffer_dir, j->ffmpeg, *lang, r.end_us, to, to > now - EK_GIVE_UP, kills, COUNT(kills));
        if (n < 0) continue;
        put = 0;
        for (i32 k = 0; k < n; k++) {
            char note[80], detail[256] = "";
            ek_kill_note(kills[k], note, sizeof note);
            marker_detail_add(detail, "side", kills[k].side);
            marker_detail_add(detail, "weapon", kills[k].weapon);
            i64 t = r.spawn_us + kills[k].secs * 1000000ll + 500000;
            if (!ek_marked(t, 1500000, note, detail)) ek_post(t, AM_KILL, 4, note, detail), put++;
        }
        log_line("markers: Escape from Tarkov: %d kills from the kill list of the raid that ended %lld", put, r.end_us);
        if (done.raid_count == COUNT(done.raids)) memmove(done.raids, done.raids + 1, sizeof done.raids[0] * --done.raid_count);
        done.raids[done.raid_count++] = r.spawn_us;
        changed = true;
    }
    if (j->events & AM_KILL && !ocr) log_line("markers: Escape from Tarkov: no kill list reading for the game's language \"%s\"", j->lang);
    if (changed) ek_done_write(j->local_dir, &done);
    CoUninitialize();
    free(j);
    return 0;
}

// From the auto markers worker: every EK_EVERY while Tarkov's markers are on, a pass on a thread of its own unless the
// last is still going.
static void eftkills_poll()
{
    u32 ev = g_am.events[AMG_TARKOV];
    if (!ev || !g_ek.eft_dir[0] || now_us() < g_ek.next) return;
    g_ek.next = now_us() + EK_EVERY;
    if (g_ek.thread) {
        if (WaitForSingleObject(g_ek.thread, 0) == WAIT_TIMEOUT) return;
        CloseHandle(g_ek.thread);
        g_ek.thread = 0;
    }
    EkJob *j = (EkJob *)calloc(1, sizeof(EkJob));
    if (!j) return;
    AcquireSRWLockShared(&g_ek.lock);
    wcscpy_s(j->eft_dir, g_ek.eft_dir);
    j->buffer_dir = g_ek.buffer_dir, j->ffmpeg = g_ek.ffmpeg, j->local_dir = g_ek.local_dir;
    str_set(j->lang, g_ek.lang);
    ReleaseSRWLockShared(&g_ek.lock);
    j->events = ev;
    g_ek.thread = thread_start(ek_thread, j);
}

// The UI language in Game.ini's "Language", "" if it can't be read.
static void ek_game_lang(char (&out)[16])
{
    out[0] = 0;
    wchar_t app[MAX_PATH], ini[MAX_PATH];
    DWORD k = GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH);
    if (!k || k >= MAX_PATH) return;
    swprintf(ini, MAX_PATH, L"%s\\Battlestate Games\\Escape from Tarkov\\Settings\\Game.ini", app);
    char *text = read_file(ini, 0);
    const char *at = text ? strstr(text, "\"Language\"") : 0;
    if (at && (at = strchr(at + 10, ':')) && (at = strchr(at, '"'))) {
        size_t l = strcspn(++at, "\"\r\n");
        if (l < sizeof out) memcpy(out, at, l), out[l] = 0;
    }
    free(text);
}

// The game's folder, where the buffer and ffmpeg are, and the game's language. From automarks_apply. Main thread.
static void eftkills_apply(const wchar_t *eft_dir)
{
    Path ffmpeg = {};
    if (eft_dir[0]) ffmpeg_find(g_app.config.ffmpeg, &ffmpeg);
    char lang[16];
    ek_game_lang(lang);
    AcquireSRWLockExclusive(&g_ek.lock);
    wcscpy_s(g_ek.eft_dir, eft_dir);
    g_ek.buffer_dir = g_app.config.buffer_dir;
    g_ek.ffmpeg = ffmpeg;
    g_ek.local_dir = g_app.paths.local_dir;
    str_set(g_ek.lang, lang);
    ReleaseSRWLockExclusive(&g_ek.lock);
}
