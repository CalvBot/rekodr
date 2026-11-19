// The window: created hidden at startup and shown on demand, so it appears instantly. Dear ImGui on D3D11.
// It draws only while shown, and then only for a few frames after input or a change (ui_wake), or continuously
// while something animates (playback, progress). Closing it hides it.
//
// UI rules (SPEC.md): no tooltips or inline key hints (shortcuts are in Help, F1); controls never move after a
// click; labels explain themselves.

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct Gpu {
    ID3D11Device           *device;
    ID3D11DeviceContext    *context;
    IDXGISwapChain1        *swapchain;
    ID3D11RenderTargetView *target;
    u32                     width, height;
};

static Gpu g_gpu;

// Colours. One dark theme: a near-black page, a darker top bar, raised surfaces for cards and fields, and one
// accent (clipback red unless Options picks another) for the primary action and the selected item. Recording and
// deleting stay red whatever the accent. Blue is for zoom keys and focus.
static ImVec4 rgb(u32 c, f32 a = 1) { return ImVec4(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a); }
static ImU32 col32(u32 c, f32 a = 1) { return ImGui::GetColorU32(rgb(c, a)); }
static const u32 COL_BG = 0x101114,      // the page
                 COL_SIDEBAR = 0x0b0c0e, // the top bar
                 COL_PANEL = 0x18191d,   // cards, the timeline
                 COL_PANEL2 = 0x222429,  // fields, secondary buttons, popups
                 COL_HOVER = 0x2c2f35,   // anything under the mouse
                 COL_LINE = 0x2a2c32,    // hairlines
                 COL_TEXT = 0xeceef1, COL_DIM = 0x8e939c, COL_FAINT = 0x5d626b,
                 COL_RED = 0xf0434f, COL_RED_SOFT = 0x3a1c20,
                 COL_BLUE = 0x5b9cff, COL_OK = 0x3fbf74, COL_WARN = 0xe8a33d;
// What a sound track is, the same colour everywhere it shows (the Apps pills, the editor's lanes).
// The toasts' sizes at 100%, shared by the desktop ones (toast.cpp) and the session popup (sessions.cpp): a tile
// beside a title over a line, then a row of buttons; a marker's note field goes between them.
static const f32 TOAST_W = 400, TOAST_PAD = 16, TOAST_GAP = 12, TOAST_MARGIN = 16, TOAST_ROUND = 8, TOAST_TILE = 40,
                 TOAST_BUTTON_W = 84, TOAST_BUTTON_H = 30, TOAST_BUTTON_GAP = 8, TOAST_BUTTON_ROUND = 7, TOAST_FIELD_H = 34,
                 TOAST_DOT = 22, TOAST_DOT_GAP = 10;

// The accent choices (Options, Look), and the colours in use, set by ui_apply_accent.
struct Accent { const char *name; u32 base, hover; };
static const Accent ACCENTS[] = {
    {"Red", 0xf0434f, 0xff5b66},    {"Orange", 0xf07a2e, 0xff9150}, {"Yellow", 0xe0b31f, 0xf2c642},
    {"Green", 0x34b36b, 0x49c97f},  {"Teal", 0x1fb0a8, 0x35c8c0},   {"Blue", 0x3d8bfd, 0x5b9cff},
    {"Purple", 0x8b5cf6, 0xa07cff}, {"Pink", 0xe8509a, 0xf56cad},
};
enum { ACCENT_COUNT = sizeof ACCENTS / sizeof ACCENTS[0] };
static u32 COL_ACCENT = 0xf0434f, COL_ACCENT_HOVER = 0xff5b66, COL_ACCENT_SOFT = 0x3a1c20;

static void ui_apply_accent(i32 i)
{
    if (i < 0 || i >= ACCENT_COUNT) i = 0;
    COL_ACCENT = ACCENTS[i].base;
    COL_ACCENT_HOVER = ACCENTS[i].hover;
    // A dark wash of the accent: a quarter of it over a card.
    u32 r = (((COL_ACCENT >> 16) & 255) + 3 * ((COL_PANEL >> 16) & 255)) / 4;
    u32 g = (((COL_ACCENT >> 8) & 255) + 3 * ((COL_PANEL >> 8) & 255)) / 4;
    u32 b = ((COL_ACCENT & 255) + 3 * (COL_PANEL & 255)) / 4;
    COL_ACCENT_SOFT = (r << 16) | (g << 8) | b;
}

// Fonts: Segoe UI with the Segoe Fluent Icons glyphs merged in, so a label can start with an icon (ICON_*).
// g_font 14 px, g_font_bold 14 px semibold, g_font_small 12.5 px, g_font_big 18 px semibold (card and dialog
// titles), g_font_title 26 px semibold (screen titles), g_font_icon 18 px icons alone (icon buttons).
static ImFont *g_font, *g_font_bold, *g_font_small, *g_font_big, *g_font_title, *g_font_icon;
static f32 g_dpi_scale = 1;

// Segoe Fluent Icons code points, as UTF-8
#define ICON_LIBRARY  "\xEE\xA3\xB1" // E8F1
#define ICON_TIMELINE "\xEE\xA0\x9C" // E81C history
#define ICON_SETTINGS "\xEE\x9C\x93" // E713
#define ICON_HELP     "\xEE\xA2\x97" // E897
#define ICON_PLAY     "\xEE\x9D\xA8" // E768
#define ICON_PAUSE    "\xEE\x9D\xA9" // E769
#define ICON_RECORD   "\xEE\x9F\x88" // E7C8
#define ICON_SEARCH   "\xEE\x9C\xA1" // E721
#define ICON_SAVE     "\xEE\x9D\x8E" // E74E
#define ICON_EDIT     "\xEE\x9C\x8F" // E70F
#define ICON_DELETE   "\xEE\x9D\x8D" // E74D
#define ICON_SHARE    "\xEE\x9C\xAD" // E72D
#define ICON_FOLDER   "\xEE\xA2\xB7" // E8B7
#define ICON_RENAME   "\xEE\xA2\xAC" // E8AC
#define ICON_CLOSE    "\xEE\x9C\x91" // E711
#define ICON_LEFT     "\xEE\x9D\xAB" // E76B chevron
#define ICON_RIGHT    "\xEE\x9D\xAC" // E76C chevron
#define ICON_DOWN     "\xEE\x9C\x8D" // E70D chevron
#define ICON_BACK     "\xEE\x9C\xAB" // E72B arrow
#define ICON_ZOOM_IN  "\xEE\xA2\xA3" // E8A3
#define ICON_ZOOM_OUT "\xEE\x9C\x9F" // E71F
#define ICON_ADD      "\xEE\x9C\x90" // E710
#define ICON_MUTE     "\xEE\x9D\x8F" // E74F
#define ICON_VOLUME   "\xEE\x9D\xA7" // E767
#define ICON_MORE     "\xEE\x9C\x92" // E712
#define ICON_COPY     "\xEE\xA3\x88" // E8C8
#define ICON_GAME     "\xEE\x9F\xBC" // E7FC
#define ICON_MIC      "\xEE\x9C\xA0" // E720
#define ICON_CHECK    "\xEE\x9C\xBE" // E73E
#define ICON_KEYBOARD "\xEE\x9D\xA5" // E765
#define ICON_LINK     "\xEE\x9C\x9B" // E71B
#define ICON_VIDEO    "\xEE\x9C\x94" // E714
#define ICON_WARNING  "\xEE\x9E\xBA" // E7BA
#define ICON_NOW      "\xEE\xA2\x93" // E893 skip to end
#define ICON_APPS     "\xEE\x9C\x9D" // E71D all apps
#define ICON_SLIDERS  "\xEE\xA7\xA9" // E9E9 equalizer
#define ICON_STAR      "\xEE\x9C\xB4" // E734
#define ICON_UNDO      "\xEE\x9E\xA7" // E7A7
#define ICON_WINDOW    "\xEE\x9C\xB7" // E737
#define ICON_CHAT      "\xEE\xA2\xBD" // E8BD
#define ICON_START     "\xEE\xA2\x92" // E892
#define ICON_BACK10    "\xEE\xB4\xBC" // ED3C skip back 10
#define ICON_FWD30     "\xEE\xB4\xBD" // ED3D skip forward 30
#define ICON_SOLO      "\xEE\x9F\xB6" // E7F6 headphone

// The kinds of sound track, in the order their lanes go, and how each is shown wherever tracks are: the Timeline,
// the editor, and the app categories in Options.
enum TrackKind : u8 { KIND_MIC, KIND_GAME, KIND_CHAT, KIND_APP, KIND_MIX, KIND_COUNT };
struct TrackKindLook { const char *name, *icon; u32 rgb; };
static const TrackKindLook KIND_LOOK[KIND_COUNT] = {
    {"Mic", ICON_MIC, 0xf06fa0}, {"Game", ICON_GAME, 0x3fbf74}, {"Chat", ICON_CHAT, 0xb07cff},
    {"Apps", ICON_APPS, 0x5b9cff}, {"Mix", ICON_VOLUME, 0xeceef1},
};

static void library_window_shown(bool shown); // library.cpp: rescans when shown, frees players and textures when hidden

static void ui_wake(i32 frames = 3)
{
    if (g_app.ui.frames_left < frames) g_app.ui.frames_left = frames;
}

static void gpu_create_target()
{
    ID3D11Texture2D *back = 0;
    g_gpu.swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_gpu.device->CreateRenderTargetView(back, 0, &g_gpu.target);
    back->Release();
}

static bool gpu_init(HWND hwnd)
{
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    // Video support lets the player's Media Foundation decode straight into textures on this device; without it the
    // window still works, but nothing plays.
    HRESULT hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 1,
                                   D3D11_SDK_VERSION, &g_gpu.device, 0, &g_gpu.context);
    if (FAILED(hr))
        hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                               D3D11_SDK_VERSION, &g_gpu.device, 0, &g_gpu.context);
    if (FAILED(hr)) {
        log_line("D3D11 device failed (0x%08x)", (u32)hr);
        return false;
    }
    IDXGIDevice *dxgi_device = 0;
    IDXGIAdapter *adapter = 0;
    IDXGIFactory2 *factory = 0;
    g_gpu.device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    dxgi_device->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(g_gpu.device, hwnd, &d, 0, 0, &g_gpu.swapchain);
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    adapter->Release();
    dxgi_device->Release();
    if (FAILED(hr)) {
        log_line("swap chain failed (0x%08x)", (u32)hr);
        return false;
    }
    gpu_create_target();
    return true;
}

static void gpu_resize(u32 w, u32 h)
{
    if (!g_gpu.swapchain || w == 0 || h == 0 || (w == g_gpu.width && h == g_gpu.height)) return;
    if (g_gpu.target) g_gpu.target->Release();
    g_gpu.target = 0;
    g_gpu.swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    gpu_create_target();
    g_gpu.width = w;
    g_gpu.height = h;
}

// The icon font draws each icon somewhere in a square, and no two sit quite alike: a magnifier hangs low, a
// camera rides high. So that code can centre an icon by its line like any text, every icon's ink is moved to the
// middle: across, of its own advance; down, of `mid` (the middle of the line for icons alone, and where the
// middle of the letters is for icons that sit in a line of text).
static void ui_center_icons(ImFont *font, f32 mid)
{
    if (!font) return;
    for (ImFontGlyph &g : font->Glyphs) {
        if (g.Codepoint < 0xE700 || g.Codepoint > 0xF8FF) continue;
        f32 w = g.X1 - g.X0, h = g.Y1 - g.Y0;
        g.X0 = floorf((g.AdvanceX - w) / 2 + 0.5f);
        g.X1 = g.X0 + w;
        g.Y0 = floorf(mid - h / 2 + 0.5f);
        g.Y1 = g.Y0 + h;
    }
}

static void ui_load_fonts(f32 scale)
{
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    char regular[MAX_PATH * 3], semibold[MAX_PATH * 3];
    char dir[MAX_PATH * 3];
    wide_to_utf8(windir, dir, sizeof dir);
    snprintf(regular, sizeof regular, "%s\\Fonts\\segoeui.ttf", dir);
    snprintf(semibold, sizeof semibold, "%s\\Fonts\\seguisb.ttf", dir);

    char icons[MAX_PATH * 3];
    snprintf(icons, sizeof icons, "%s\\Fonts\\SegoeIcons.ttf", dir);
    if (GetFileAttributesA(icons) == INVALID_FILE_ATTRIBUTES) // Windows 10: the older icon font has the same code points
        snprintf(icons, sizeof icons, "%s\\Fonts\\segmdl2.ttf", dir);

    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0x25CF, 0x25CF, 0}; // Latin-1, dashes, arrows, *
    static const ImWchar icon_ranges[] = {0xE700, 0xF8FF, 0};
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    ImFontConfig merge;
    merge.MergeMode = true;
    merge.OversampleH = 2;
    merge.PixelSnapH = true;
    auto add = [&](const char *file, f32 px, f32 icon_px) {
        ImFont *f = io.Fonts->AddFontFromFileTTF(file, px * scale, &cfg, ranges);
        if (f) io.Fonts->AddFontFromFileTTF(icons, icon_px * scale, &merge, icon_ranges);
        return f;
    };
    g_font = add(regular, 14, 14);
    g_font_bold = add(semibold, 14, 14);
    g_font_small = add(regular, 12.5f, 12.5f);
    g_font_big = add(semibold, 18, 17);
    g_font_title = add(semibold, 26, 22);
    ImFontConfig alone;
    alone.OversampleH = 2;
    alone.PixelSnapH = true;
    g_font_icon = io.Fonts->AddFontFromFileTTF(icons, 17 * scale, &alone, icon_ranges);
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_bold) g_font_bold = g_font;
    if (!g_font_small) g_font_small = g_font;
    if (!g_font_big) g_font_big = g_font_bold;
    if (!g_font_title) g_font_title = g_font_big;
    if (!g_font_icon) g_font_icon = g_font;
    io.Fonts->Build();
    // Segoe UI's letters sit a little under the middle of their line: about 0.6 px at 100%
    ImFont *text_fonts[] = {g_font, g_font_bold, g_font_small, g_font_big, g_font_title};
    for (ImFont *f : text_fonts)
        if (f != g_font_icon) ui_center_icons(f, f->FontSize / 2 + 0.6f * scale * f->FontSize / (14 * scale));
    ui_center_icons(g_font_icon, g_font_icon->FontSize / 2);
    ImGui_ImplDX11_InvalidateDeviceObjects(); // the font texture is remade on the next frame
}

static void ui_style(f32 scale)
{
    ImGuiStyle &s = ImGui::GetStyle();
    s = ImGuiStyle();
    // No outlines anywhere: controls are told apart by fill, not by boxes.
    s.WindowRounding = 10;
    s.FrameRounding = 7;
    s.ChildRounding = 10;
    s.PopupRounding = 8;
    s.GrabRounding = 7;
    s.TabRounding = 7;
    s.ScrollbarRounding = 7;
    s.ScrollbarSize = 10;
    s.GrabMinSize = 12;
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 10);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.WindowPadding = ImVec2(20, 18);
    s.PopupBorderSize = 1; // popups sit over cards of the same colour
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.FrameBorderSize = 0;
    s.SeparatorTextBorderSize = 1;
    s.CellPadding = ImVec2(10, 7);
    s.SelectableTextAlign = ImVec2(0, 0.5f);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = rgb(COL_BG);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgb(COL_PANEL2);
    c[ImGuiCol_Border] = rgb(COL_LINE);
    c[ImGuiCol_Text] = rgb(COL_TEXT);
    c[ImGuiCol_TextDisabled] = rgb(COL_DIM);
    c[ImGuiCol_FrameBg] = rgb(COL_PANEL2);
    c[ImGuiCol_FrameBgHovered] = rgb(COL_HOVER);
    c[ImGuiCol_FrameBgActive] = rgb(COL_HOVER);
    c[ImGuiCol_Button] = rgb(COL_PANEL2);
    c[ImGuiCol_ButtonHovered] = rgb(COL_HOVER);
    c[ImGuiCol_ButtonActive] = rgb(COL_LINE);
    c[ImGuiCol_Header] = rgb(COL_PANEL2);
    c[ImGuiCol_HeaderHovered] = rgb(COL_HOVER);
    c[ImGuiCol_HeaderActive] = rgb(COL_HOVER);
    c[ImGuiCol_CheckMark] = rgb(COL_ACCENT);
    c[ImGuiCol_SliderGrab] = rgb(COL_TEXT);
    c[ImGuiCol_SliderGrabActive] = rgb(COL_TEXT);
    c[ImGuiCol_Separator] = rgb(COL_LINE);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableBorderLight] = rgb(COL_LINE);
    c[ImGuiCol_TableBorderStrong] = rgb(COL_LINE);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_TextSelectedBg] = rgb(COL_BLUE, 0.35f);
    c[ImGuiCol_NavCursor] = rgb(COL_BLUE);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(COL_HOVER);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(COL_FAINT);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(COL_DIM);
    s.ScaleAllSizes(scale);
}

static void ui_set_scale(f32 scale)
{
    g_dpi_scale = scale;
    ui_load_fonts(scale);
    ui_style(scale);
}

static void ui_frame();

static LRESULT ui_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        ui_wake();
        return 1;
    }
    switch (msg) {
    case WM_SIZE: {
        gpu_resize(LOWORD(lp), HIWORD(lp));
        ui_wake();
        // While an edge is dragged Windows runs its own loop and ours never gets to draw, so the old picture was
        // stretched until the mouse let go. Draw here, at the new size.
        static bool drawing;
        if (!drawing && g_app.ui.shown && g_gpu.target && wp != SIZE_MINIMIZED && ImGui::GetCurrentContext() &&
            !ImGui::GetCurrentContext()->WithinFrameScope) {
            drawing = true;
            ui_frame();
            drawing = false;
        }
        return 0;
    }
    case WM_CLOSE:
        DEV_LOG("window closed (hidden)");
        g_app.timeline.quick = false;
        ShowWindow(hwnd, SW_HIDE);
        g_app.ui.shown = false;
        library_window_shown(false);
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        SetWindowPos(hwnd, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ui_set_scale(HIWORD(wp) / 96.f);
        ui_wake();
        return 0;
    }
    case WM_SYSCOMMAND:
        // Alt alone would open the window menu and take the next key; Alt is held to zoom the timelines
        if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) return 0;
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *m = (MINMAXINFO *)lp;
        m->ptMinTrackSize.x = (LONG)(850 * g_dpi_scale);
        m->ptMinTrackSize.y = (LONG)(560 * g_dpi_scale);
        return 0;
    }
    }
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ui_wake();
    if (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ui_wake();
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The work area of the monitor furthest right, where a test copy (CLIPBACK_HOME) puts its window and toasts.
static bool rightmost_monitor(RECT *work, HMONITOR *mon)
{
    struct Best { RECT work; HMONITOR mon; } best = {{LONG_MIN}, 0};
    EnumDisplayMonitors(0, 0, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        MONITORINFO mi = {sizeof mi};
        Best *b = (Best *)p;
        if (GetMonitorInfoW(m, &mi) && mi.rcWork.left > b->work.left) *b = {mi.rcWork, m};
        return TRUE;
    }, (LPARAM)&best);
    if (!best.mon) return false;
    *work = best.work;
    *mon = best.mon;
    return true;
}

static bool ui_init()
{
    HINSTANCE inst = GetModuleHandleW(0);
    WNDCLASSEXW wc = {sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = diag_wndproc<ui_wndproc>;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(0, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0x10, 0x11, 0x14));
    wc.lpszClassName = L"clipback.window";
    RegisterClassExW(&wc);

    // Centered on the main monitor, sized for it. A test copy (CLIPBACK_HOME) starts on the rightmost monitor instead,
    // away from the user's work.
    f32 scale = GetDpiForSystem() / 96.f;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HMONITOR mon;
    UINT dx, dy;
    if (is_test_copy() && rightmost_monitor(&work, &mon) && GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy) == S_OK)
        scale = dx / 96.f;
    i32 w = ImMin((i32)(1280 * scale), (i32)(work.right - work.left)), h = ImMin((i32)(800 * scale), (i32)(work.bottom - work.top));
    i32 x = work.left + (work.right - work.left - w) / 2, y = work.top + (work.bottom - work.top - h) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"clipback", WS_OVERLAPPEDWINDOW, x, y, w, h, 0, 0, inst, 0);
    if (!hwnd) return false;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    g_app.ui.hwnd = hwnd;

    if (!gpu_init(hwnd)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = 0; // nothing to remember: the layout is fixed
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true; // popups have no title bar, so none of them can be dragged
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_gpu.device, g_gpu.context);
    ui_apply_accent(g_app.config.accent);
    ui_set_scale(GetDpiForWindow(hwnd) / 96.f);
    return true;
}

static void ui_show(Screen screen)
{
    Ui &ui = g_app.ui;
    DEV_LOG("show window, screen %d (was %s)", (int)screen, ui.shown ? "shown" : "hidden");
    ui.screen = screen;
    g_app.timeline.quick = false;
    // A test copy (CLIPBACK_HOME) shows itself without taking the foreground, so tests leave the user's work alone
    bool test = is_test_copy();
    if (!ui.shown) {
        ShowWindow(ui.hwnd, IsZoomed(ui.hwnd) ? SW_SHOWMAXIMIZED : test ? SW_SHOWNOACTIVATE : SW_SHOW);
        ui.shown = true;
        library_window_shown(true);
    }
    if (IsIconic(ui.hwnd)) ShowWindow(ui.hwnd, test ? SW_SHOWNOACTIVATE : SW_RESTORE);
    // A second launch (Stream Deck, hotkey) gave us the right to take the foreground; the tray did too.
    if (!test) SetForegroundWindow(ui.hwnd);
    ui_wake(4);
}

// ---------------------------------------------------------------------------------------------------------------
// Kit: the controls every screen is built from, so the screens look like one app. Rules they keep:
// - No outlines. A control is a filled shape; the fill says what it is (accent = the main action, grey = an
//   ordinary one, none = a quiet one that only shows a fill under the mouse).
// - One accent (clipback red) per screen region at most: the primary button, the selected chip or nav item.
// - Sizes never depend on state: a segmented control sizes every segment to the widest label, a button that
//   changes its label is given a width.

enum KitKind { KIT_SECONDARY, KIT_PRIMARY, KIT_GHOST, KIT_DANGER };

static f32 kit_px(f32 v) { return v * g_dpi_scale; }

static const char *const SCREEN_NAMES[] = {"library", "timeline", "editor", "options"};
static const char *screen_name(Screen s) { return (u32)s < COUNT(SCREEN_NAMES) ? SCREEN_NAMES[s] : "?"; }

// Dev mode: a control was used. label is a widget label or id: its icon and "##..." are left out ("##play" = "play").
static void dev_click(const char *what, const char *label, const char *extra = "")
{
    if (!g_dev) return;
    if (!label) label = "";
    while ((u8)label[0] == 0xEE || (u8)label[0] == 0xEF || label[0] == ' ') label += (u8)label[0] == ' ' ? 1 : 3;
    char text[160];
    const char *hash = strstr(label, "##");
    if (hash == label) str_set(text, label + 2);
    else snprintf(text, sizeof text, "%.*s", hash ? (int)(hash - label) : (int)strlen(label), label);
    log_line("%s: %s \"%s\"%s%s", screen_name(g_app.ui.screen), what, text, extra[0] ? " " : "", extra);
}

// A text button. size 0 = fit the label.
// A click outside an open list or menu closes it and also does what was clicked: ImGui by itself would only close
// it and make the click be done again. ui_popups_click_through, at the start of a frame, closes the list before
// anything is drawn, so the click lands as if it had never been open. The one exception is the control the list
// dropped from: clicking that again only closes it, and kit_closed_here tells that control so.
static ImGuiID g_popup_opener;      // the control whose click opened the list that is open
static ImGuiID g_popup_top;         // the list that was on top last frame
static ImGuiID g_popup_closed_from; // the opener of a list closed by the click that is still held

static void ui_popups_click_through()
{
    ImGuiContext &g = *ImGui::GetCurrentContext();
    // The opener stays told until the button that closed its list is let go: its click is the release
    if (g_popup_closed_from && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        g_popup_closed_from = 0;
    ImGuiID top = g.OpenPopupStack.Size ? g.OpenPopupStack.back().PopupId : 0;
    if (top && top != g_popup_top && g.OpenPopupStack.Size == 1)
        g_popup_opener = g.LastActiveIdTimer < 0.5f ? g.LastActiveId : 0; // newly open: whatever was just pressed
    g_popup_top = top;
    if (!top || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::GetTopMostPopupModal()) return;
    ImGuiWindow *over = g.HoveredWindow;
    if (over && (over->RootWindow->Flags & ImGuiWindowFlags_Popup)) return; // a click in the list itself
    ImGui::ClosePopupToLevel(0, true);
    g_popup_closed_from = g_popup_opener;
    g_popup_top = 0;
}

// True for the control (by its id) that the list just closed by this click had dropped from.
static bool kit_closed_here(ImGuiID id)
{
    return id && id == g_popup_closed_from;
}

static bool kit_button(const char *label, KitKind kind = KIT_SECONDARY, ImVec2 size = ImVec2(0, 0), bool enabled = true)
{
    u32 bg = COL_PANEL2, hover = COL_HOVER, text = COL_TEXT;
    f32 bg_a = 1;
    switch (kind) {
    case KIT_PRIMARY: bg = COL_ACCENT; hover = COL_ACCENT_HOVER; text = 0xffffff; break;
    case KIT_GHOST: bg_a = 0; break;
    case KIT_DANGER: bg = COL_RED_SOFT; hover = 0x4a2025; text = 0xff8a92; break;
    default: break;
    }
    if (!enabled) { // plainly off: a quiet fill and faint text, whatever kind it is
        bg = hover = COL_PANEL2;
        text = COL_FAINT;
        bg_a = kind == KIT_GHOST ? 0.0f : 1.0f;
    }
    ImGui::PushStyleColor(ImGuiCol_Button, rgb(bg, bg_a));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(hover, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(text));
    if (kind == KIT_PRIMARY) ImGui::PushFont(g_font_bold);
    if (!enabled) ImGui::BeginDisabled();
    bool clicked = ImGui::Button(label, size) && !kit_closed_here(ImGui::GetItemID());
    if (!enabled) ImGui::EndDisabled();
    if (clicked && enabled) dev_click("button", label);
    if (kind == KIT_PRIMARY) ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return clicked && enabled;
}

static void kit_tip(const char *what, const char *keys = 0);

// The tooltip the next kit_icon_button shows: kit_next_tip("Play", "Space") just before drawing it.
static const char *g_next_tip, *g_next_tip_keys;
static void kit_next_tip(const char *what, const char *keys = 0)
{
    g_next_tip = what;
    g_next_tip_keys = keys;
}

// A square button showing one icon (ICON_*). id must be unique in the window ("##play"). active draws it selected.
static bool kit_icon_button(const char *icon, const char *id, KitKind kind = KIT_GHOST, bool enabled = true,
                            bool active = false, f32 size = 0)
{
    const char *tip = g_next_tip, *tip_keys = g_next_tip_keys;
    g_next_tip = g_next_tip_keys = 0;
    if (size <= 0) size = ImGui::GetFrameHeight();
    ImVec2 a = ImGui::GetCursorScreenPos();
    if (!enabled) ImGui::BeginDisabled();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size)) && !kit_closed_here(ImGui::GetItemID());
    if (!enabled) ImGui::EndDisabled();
    if (tip) kit_tip(tip, tip_keys);
    if (clicked && enabled) dev_click("button", id);
    bool hot = enabled && ImGui::IsItemHovered(), held = hot && ImGui::IsItemActive();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 z(a.x + size, a.y + size);
    u32 fill = 0, ink = COL_TEXT;
    f32 fill_a = 1;
    switch (kind) {
    case KIT_PRIMARY: fill = hot ? COL_ACCENT_HOVER : COL_ACCENT; ink = 0xffffff; break;
    case KIT_SECONDARY: fill = hot ? COL_HOVER : COL_PANEL2; break;
    case KIT_DANGER: fill = hot ? 0x4a2025 : COL_RED_SOFT; ink = 0xff8a92; break;
    default:
        if (active) fill = COL_HOVER;
        else if (hot) fill = COL_PANEL2;
        else fill_a = 0;
        if (!active && !hot) ink = 0xc4c8cf;
        break;
    }
    if (held) fill_a *= 0.8f;
    if (fill_a > 0) dl->AddRectFilled(a, z, col32(fill, fill_a), ImGui::GetStyle().FrameRounding);
    ImGui::PushFont(g_font_icon);
    ImVec2 ts = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(floorf(a.x + (size - ts.x) / 2), floorf(a.y + (size - ts.y) / 2)), col32(ink, enabled ? 1 : 0.35f), icon);
    ImGui::PopFont();
    return clicked && enabled;
}

// Is the key named [s, e) held right now? Names as the Help window writes them: Ctrl, Shift, Space, Page Up, Left,
// F1, A, 5, + and so on.
static bool kit_key_held(const char *s, const char *e)
{
    i32 n = (i32)(e - s);
    char k[16];
    if (n <= 0 || n >= (i32)sizeof k) return false;
    for (i32 i = 0; i < n; i++) k[i] = (char)tolower((unsigned char)s[i]);
    k[n] = 0;
    ImGuiIO &io = ImGui::GetIO();
    if (!strcmp(k, "ctrl")) return io.KeyCtrl;
    if (!strcmp(k, "shift")) return io.KeyShift;
    if (!strcmp(k, "alt")) return io.KeyAlt;
    if (!strcmp(k, "+")) return ImGui::IsKeyDown(ImGuiKey_Equal) || ImGui::IsKeyDown(ImGuiKey_KeypadAdd);
    if (!strcmp(k, "-")) return ImGui::IsKeyDown(ImGuiKey_Minus) || ImGui::IsKeyDown(ImGuiKey_KeypadSubtract);
    if (!strcmp(k, "enter")) return ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter);
    if (n == 1 && k[0] >= 'a' && k[0] <= 'z') return ImGui::IsKeyDown((ImGuiKey)(ImGuiKey_A + (k[0] - 'a')));
    if (n == 1 && k[0] >= '0' && k[0] <= '9') return ImGui::IsKeyDown((ImGuiKey)(ImGuiKey_0 + (k[0] - '0')));
    if (k[0] == 'f' && n <= 3 && atoi(k + 1) >= 1 && atoi(k + 1) <= 24)
        return ImGui::IsKeyDown((ImGuiKey)(ImGuiKey_F1 + atoi(k + 1) - 1));
    static const struct { const char *name; ImGuiKey key; } named[] = {
        {"space", ImGuiKey_Space},   {"esc", ImGuiKey_Escape},          {"delete", ImGuiKey_Delete},
        {"home", ImGuiKey_Home},     {"end", ImGuiKey_End},             {"page up", ImGuiKey_PageUp},
        {"page down", ImGuiKey_PageDown}, {"left", ImGuiKey_LeftArrow}, {"right", ImGuiKey_RightArrow},
        {"up", ImGuiKey_UpArrow},    {"down", ImGuiKey_DownArrow},      {"tab", ImGuiKey_Tab},
    };
    for (auto &m : named)
        if (!strcmp(k, m.name)) return ImGui::IsKeyDown(m.key);
    return false;
}

// A key cap with its top-left at (x, y), in the small font: raised, and lit in the accent while the key is held.
// Returns its width.
static f32 kit_keycap(ImDrawList *dl, f32 x, f32 y, const char *s, const char *e)
{
    ImGui::PushFont(g_font_small);
    f32 sh = ImGui::GetTextLineHeight(), cap_h = sh + kit_px(6);
    ImVec2 ts = ImGui::CalcTextSize(s, e);
    f32 cw = fmaxf(ts.x + kit_px(14), cap_h);
    bool lit = kit_key_held(s, e);
    f32 drop = lit ? kit_px(1) : 0; // pressed in a little
    dl->AddRectFilled(ImVec2(x, y + kit_px(2)), ImVec2(x + cw, y + cap_h + kit_px(2)), col32(0x0c0d10), kit_px(5));
    dl->AddRectFilled(ImVec2(x, y + drop), ImVec2(x + cw, y + cap_h + drop), col32(lit ? COL_ACCENT : COL_HOVER), kit_px(5));
    dl->AddText(ImVec2(floorf(x + (cw - ts.x) / 2), floorf(y + drop + (cap_h - sh) / 2)), col32(lit ? 0xffffff : COL_TEXT), s, e);
    ImGui::PopFont();
    return cw;
}

// A tooltip for the item just drawn: what it does and, if it has one, its key ("Space", "Ctrl+F") as caps. Shown only
// with Options > General > Tooltips on; that switch is the one place tooltips are turned off.
static void kit_tip(const char *what, const char *keys)
{
    if (!g_app.config.tooltips || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kit_px(10), kit_px(7)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kit_px(7));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(COL_PANEL2));
    if (ImGui::BeginTooltip()) {
        ImGui::PushFont(g_font_small);
        f32 cap_h = ImGui::GetTextLineHeight() + kit_px(8);
        ImGui::PopFont();
        ImVec2 a = ImGui::GetCursorScreenPos();
        f32 th = ImGui::GetTextLineHeight(), h = keys ? fmaxf(th, cap_h) : th;
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + floorf((h - th) / 2)));
        ImGui::TextUnformatted(what);
        f32 x = a.x + ImGui::CalcTextSize(what).x + kit_px(10);
        if (keys) {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            for (const char *p = keys; *p;) {
                const char *q = p;
                while (*q && !(*q == '+' && q > p)) q++; // "Ctrl++" is Ctrl and +
                x += kit_keycap(dl, x, a.y, p, q) + kit_px(3);
                p = *q ? q + 1 : q;
            }
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::Dummy(ImVec2(x - a.x - (keys ? kit_px(3) : kit_px(10)), h));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

// A row of options sharing one rounded track; the chosen one is lifted. Every segment is as wide as the widest
// label (or item_w), so nothing moves. Returns true when *sel changed.
static bool kit_segmented(const char *id, const char *const *labels, i32 n, i32 *sel, f32 item_w = 0)
{
    ImGuiStyle &st = ImGui::GetStyle();
    if (item_w <= 0)
        for (i32 i = 0; i < n; i++) item_w = fmaxf(item_w, ImGui::CalcTextSize(labels[i]).x + 2 * st.FramePadding.x);
    f32 h = ImGui::GetFrameHeight(), pad = kit_px(3);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + n * item_w + 2 * pad, a.y + h), col32(COL_PANEL2), st.FrameRounding);
    bool changed = false;
    ImGui::PushID(id);
    for (i32 i = 0; i < n; i++) {
        ImVec2 p(a.x + pad + i * item_w, a.y + pad);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##seg", ImVec2(item_w, h - 2 * pad));
        bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        if (clicked && *sel != i) {
            *sel = i;
            changed = true;
            dev_click(id, labels[i]);
        }
        bool on = *sel == i;
        if (on) dl->AddRectFilled(p, ImVec2(p.x + item_w, p.y + h - 2 * pad), col32(0x3a3d44), st.FrameRounding - 2);
        else if (hot) dl->AddRectFilled(p, ImVec2(p.x + item_w, p.y + h - 2 * pad), col32(COL_HOVER), st.FrameRounding - 2);
        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText(ImVec2(floorf(p.x + (item_w - ts.x) / 2), floorf(a.y + (h - ts.y) / 2)), col32(on || hot ? COL_TEXT : COL_DIM), labels[i]);
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(a);
    ImGui::Dummy(ImVec2(n * item_w + 2 * pad, h));
    return changed;
}

// A rounded filter pill. active = white with dark text. count is drawn after the label, dimmer. w 0 = fit.
static bool kit_chip(const char *id_label, bool active, const char *count = 0, f32 w = 0)
{
    const char *end = ImGui::FindRenderedTextEnd(id_label);
    ImVec2 ls = ImGui::CalcTextSize(id_label, end);
    f32 gap = kit_px(8), padx = kit_px(14), h = ImGui::GetFrameHeight();
    ImVec2 cs = count ? ImGui::CalcTextSize(count) : ImVec2(0, 0);
    if (w <= 0) w = ls.x + (count ? gap + cs.x : 0) + 2 * padx;
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id_label, ImVec2(w, h));
    if (clicked) dev_click("chip", id_label, active ? "(was on)" : "");
    bool hot = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    u32 fill = active ? COL_TEXT : hot ? COL_HOVER : COL_PANEL2;
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), col32(fill), h / 2);
    f32 y = floorf(a.y + (h - ls.y) / 2);
    dl->AddText(ImVec2(a.x + padx, y), col32(active ? COL_BG : COL_TEXT), id_label, end);
    if (count) dl->AddText(ImVec2(a.x + w - padx - cs.x, y), col32(active ? 0x5d626b : COL_DIM), count);
    return clicked;
}

// An on/off switch. Returns true when flipped.
static bool kit_toggle(const char *id, bool *v)
{
    f32 h = kit_px(20), w = kit_px(36);
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 fh = ImGui::GetFrameHeight();
    a.y += (fh - h) / 2;
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y - (fh - h) / 2));
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, fh));
    if (clicked) {
        *v = !*v;
        dev_click("switch", id, *v ? "on" : "off");
    }
    bool hot = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), col32(*v ? (hot ? COL_ACCENT_HOVER : COL_ACCENT) : (hot ? 0x45484f : 0x3a3d44)), h / 2);
    f32 r = h / 2 - kit_px(3);
    dl->AddCircleFilled(ImVec2(*v ? a.x + w - h / 2 : a.x + h / 2, a.y + h / 2), r, col32(0xffffff));
    return clicked;
}

// A search field with a magnifier inside it.
static bool kit_search(const char *id, char *buf, i32 cap, const char *hint, f32 w)
{
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kit_px(36), ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(w);
    bool changed = ImGui::InputTextWithHint(id, hint, buf, cap);
    ImGui::PopStyleVar();
    f32 fh = ImGui::GetFrameHeight(); // the field's, so before the icon font (a taller one) is pushed
    ImGui::PushFont(g_font_icon);
    ImVec2 ts = ImGui::CalcTextSize(ICON_SEARCH);
    ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + kit_px(12), floorf(a.y + (fh - ts.y) / 2)),
                                        col32(COL_DIM), ICON_SEARCH);
    ImGui::PopFont();
    return changed;
}

// A raised panel. Everything between begin and end is drawn inside it, padded.
static bool kit_card_begin(const char *id, ImVec2 size, f32 pad = 18)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(COL_PANEL));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kit_px(pad), kit_px(pad)));
    bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    return open;
}
static void kit_card_end()
{
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// Text helpers
static void kit_heading(const char *text) // a card or section title
{
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}
static void kit_label(const char *text) // a small dim caption above a group: "SOUND", "SELECTION"
{
    ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_FAINT));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
static void kit_dim(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_DIM));
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
}

// The wheel over the control just drawn steps its choice (of `count`, skipping the `disabled` bits, stopping at
// the ends). It takes the wheel while the mouse is there, so the page behind doesn't scroll as well.
static bool kit_wheel_step(i32 *index, i32 count, u32 disabled = 0)
{
    if (!ImGui::IsItemHovered()) return false;
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    f32 wheel = ImGui::GetIO().MouseWheel;
    if (wheel == 0 || ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId)) return false;
    i32 dir = wheel < 0 ? 1 : -1, i = *index + dir;
    while (i >= 0 && i < count && ((disabled >> i) & 1)) i += dir;
    if (i < 0 || i >= count) return false;
    *index = i;
    return true;
}

static void kit_align_right(f32 w)
{
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - w);
}

// A texture from BGRA pixels, for ImGui to draw (thumbnails, app icons).
static ID3D11ShaderResourceView *gpu_texture(const u32 *pixels, i32 w, i32 h)
{
    FRAME_PART("texture upload");
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {pixels, (UINT)w * 4, 0};
    ID3D11Texture2D *tex = 0;
    ID3D11ShaderResourceView *srv = 0;
    if (FAILED(g_gpu.device->CreateTexture2D(&d, &data, &tex))) return 0;
    g_gpu.device->CreateShaderResourceView(tex, 0, &srv);
    tex->Release();
    return srv;
}

// ---------------------------------------------------------------------------------------------------------------
// Screens, each in its own file. They can use everything above (theme colours, fonts, ui_wake, the kit).

#include "appicons.cpp"
#include "sessions.cpp"
#include "player.cpp"
#include "library.cpp"
#include "wave.cpp"
#include "workarea.cpp"
#include "editor.cpp"
#include "timeline.cpp"
#include "options.cpp"
#include "setup.cpp"

struct HelpRow { const char *keys, *what; };
struct HelpSection { const char *title; const HelpRow *rows; i32 count; };

// Does text contain q, ignoring case? An empty q is in everything.
static bool help_has(const char *text, const char *q)
{
    if (!*q) return true;
    for (; *text; text++) {
        i32 i = 0;
        while (q[i] && tolower((unsigned char)text[i]) == tolower((unsigned char)q[i])) i++;
        if (!q[i]) return true;
    }
    return false;
}

// Does a row (or its section) match the Help search?
static bool help_row_shown(const char *title, const HelpRow &r, const char *q)
{
    return help_has(title, q) || help_has(r.keys, q) || help_has(r.what, q);
}

// One group of the Help window, in a column w wide, with only the rows matching the search find (none: nothing drawn).
// A row's keys are written with markup: [Ctrl] is drawn as a key cap (lit while held), `clipback open` as a command,
// and anything else (mouse actions, "+", " / ") as plain text.
static void help_section(const char *title, const HelpRow *rows, i32 count, f32 w, const char *find)
{
    i32 shown = 0;
    for (i32 i = 0; i < count; i++) shown += help_row_shown(title, rows[i], find);
    if (!shown) return;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 keys_w = kit_px(168), gap = kit_px(8);
    kit_label(title);
    ImGui::Dummy(ImVec2(0, kit_px(2)));
    for (i32 i = 0; i < count; i++) {
        if (!help_row_shown(title, rows[i], find)) continue;
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImGui::PushFont(g_font_small);
        f32 sh = ImGui::GetTextLineHeight(), cap_h = sh + kit_px(6);
        f32 x = a.x, y = a.y;
        for (const char *p = rows[i].keys; *p;) {
            char close = *p == '[' ? ']' : *p == '`' ? '`' : 0;
            const char *end = close ? strchr(p + 1, close) : 0;
            if (end) {
                // A key cap (raised) or a command (flat, darker)
                ImVec2 ts = ImGui::CalcTextSize(p + 1, end);
                f32 cw = ts.x + kit_px(14);
                if (cw < cap_h) cw = cap_h;
                if (close == ']') {
                    cw = kit_keycap(dl, x, y, p + 1, end);
                } else {
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw, y + cap_h), col32(COL_BG), kit_px(5));
                    dl->AddText(ImVec2(floorf(x + kit_px(7)), floorf(y + (cap_h - sh) / 2)), col32(0xc4c8cf), p + 1, end);
                }
                x += cw + kit_px(3);
                p = end + 1;
            } else {
                // Plain text up to the next cap or command
                const char *q = p + 1;
                while (*q && *q != '[' && *q != '`') q++;
                ImGui::PushFont(g_font);
                f32 fs = ImGui::GetTextLineHeight();
                dl->AddText(ImVec2(x, floorf(y + (cap_h - fs) / 2)), col32(COL_DIM), p, q);
                x += ImGui::CalcTextSize(p, q).x + kit_px(3);
                ImGui::PopFont();
                p = q;
            }
        }
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(a.x + keys_w + gap, floorf(a.y + (cap_h - ImGui::GetTextLineHeight()) / 2)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - keys_w - gap);
        ImGui::TextUnformatted(rows[i].what);
        ImGui::PopTextWrapPos();
        f32 bottom = ImGui::GetCursorScreenPos().y;
        if (bottom < a.y + cap_h + kit_px(8)) bottom = a.y + cap_h + kit_px(8);
        ImGui::SetCursorScreenPos(ImVec2(a.x, bottom));
    }
    ImGui::Dummy(ImVec2(w, kit_px(12)));
}

static void help_window()
{
    if (g_app.ui.help_open) ImGui::OpenPopup("Help");
    static char q[64]; // the search

    // As many columns as fit (one to three) and the window's height, so it always fits the window; the rows scroll.
    f32 col_w = kit_px(372), col_gap = kit_px(40), pad = kit_px(28), margin = kit_px(24);
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    i32 cols = (i32)((vp->WorkSize.x - 2 * margin - 2 * pad + col_gap) / (col_w + col_gap));
    if (cols > 3) cols = 3;
    if (cols < 1) {
        cols = 1;
        col_w = fmaxf(kit_px(200), vp->WorkSize.x - 2 * margin - 2 * pad);
    }
    f32 win_w = cols * col_w + (cols - 1) * col_gap + 2 * pad, win_h = fminf(vp->WorkSize.y - 2 * margin, kit_px(860));
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y / 2), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, kit_px(24)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kit_px(14));
    bool open = true;
    if (ImGui::BeginPopupModal("Help", &open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        static const HelpRow anywhere[] = {
            {"`clipback save 90s`", "Save the last 90 s (or 30s, 2m, 1h ...)"},
            {"`clipback timeline`", "Open the buffer timeline"},
            {"`clipback open`", "Open this window"},
            {"`clipback toggle`", "Recording on / off"},
        };
        static const HelpRow window[] = {{"[F1]", "This help"}, {"[Esc]", "Close this help"},
                                         {"[Esc]", "Options, setting a hotkey's keys: cancel"}};
        static const HelpRow library[] = {
            {"[Ctrl]+[F]", "Search"},
            {"Mouse wheel", "Scroll"},
            {"[Page Up] / [Page Down]", "A screen up / down"},
            {"[Home] / [End]", "To the top / bottom"},
            {"[Ctrl]+wheel", "Bigger or smaller cards"},
            {"Point at a thumbnail", "Scrub through it; click to play from there"},
            {"Right-click a card", "Edit, rename, delete and more"},
            {"[F2]", "Rename the card under the mouse"},
            {"Drag a card onto a folder", "Add it to the folder"},
            {"Right-click a folder", "Rename or delete it"},
        };
        static const HelpRow player[] = {
            {"[Space]", "Play / pause"},
            {"[Left] / [Right]", "Back / forward 5 s"},
            {"[Shift]+[Left] / [Right]", "Back 10 s / forward 30 s"},
            {"[Shift]+[.] / [,]", "Faster / slower"},
            {"[M]", "Mute / unmute"},
            {"[Esc]", "Close the player"},
        };
        static const HelpRow editor[] = {
            {"[Space]", "Play / pause"},
            {"[I] / [O]", "Trim start / end at the playhead"},
            {"Click or drag on the ruler or picture strip", "Move the playhead"},
            {"[Shift]+drag there", "Set a new in and out"},
            {"Drag the in and out's band or ends", "Move them"},
            {"[Shift]+drag an end", "Catch the clip's start, end or the playhead"},
            {"[K]", "Zoom key at the playhead"},
            {"Double-click a lane", "Add a key there"},
            {"[Ctrl]+click a lane", "Add a key on the line"},
            {"Drag a key", "Move it in time and level"},
            {"[Shift]+drag a key", "Time only, catching the playhead"},
            {"[Ctrl]+drag a key", "Level only"},
            {"[Shift]+click a key", "Add it to the selection, or take it out"},
            {"Drag a flat stretch", "Its level, both keys at once"},
            {"Right-click a key", "Ease, copy, delete"},
            {"[1] to [5]", "Ease: linear, smooth, in, out, hold"},
            {"[Delete]", "Delete the selected keys"},
            {"[Ctrl]+[A]", "Select every key"},
            {"[Ctrl]+[C] / [Ctrl]+[V]", "Copy keys / paste at the playhead"},
            {"[Ctrl]+[Z] / [Ctrl]+[Y]", "Undo / redo"},
            {"Mouse wheel, middle-drag", "Pan the timeline"},
            {"[Alt]+wheel", "Zoom around the mouse"},
            {"[Alt]+[Shift]+wheel", "Zoom around the playhead"},
            {"[+] / [-]", "Zoom in / out"},
            {"Drag the work area bar", "Pan; drag its ends to zoom, double-click for all"},
            {"[Shift]+drag an end of it", "Zoom around the playhead"},
            {"Wheel over the track names", "Scroll the sound tracks"},
            {"[Shift]+wheel", "Taller or shorter lanes"},
            {"[Left] / [Right]", "Previous / next key or trim end"},
            {"[,] / [.]", "Step one frame"},
            {"[Shift]+[Left] / [Right]", "Step 1 s"},
            {"Drag the picture", "Aim the zoom (when zoomed in)"},
            {"[Esc]", "Clear the selection, then back to the library"},
        };
        static const HelpRow timeline[] = {
            {"Click or drag on the tracks", "Move the playhead"},
            {"[Shift]+drag", "Select a range"},
            {"[Shift]+click a game", "Add that part to the selection; again takes it out"},
            {"Drag the selection's band or ends", "Move them; the playhead follows"},
            {"[Shift]+drag an end", "Catch where sound, video, a game or a save starts or ends"},
            {"[Space]", "Play or pause from the playhead"},
            {"Mouse wheel", "Pan earlier or later"},
            {"[Alt]+wheel", "Zoom around the mouse"},
            {"[Alt]+[Shift]+wheel", "Zoom around the playhead"},
            {"Drag the work area bar", "Pan; drag its ends to zoom, double-click for all"},
            {"[Shift]+drag an end of it", "Zoom around the playhead"},
            {"Wheel over the names", "Scroll the sound tracks"},
            {"[Shift]+wheel over the tracks", "Taller or shorter tracks"},
            {"Wheel over the preview", "Zoom the picture in at the mouse"},
            {"Middle drag the preview", "Move around the zoomed picture"},
            {"Right or middle drag", "Pan earlier or later"},
            {"[Left] / [Right]", "Move the selection 1 s (with Shift 10 s)"},
            {"[I] / [O] or [Q] / [E]", "Set the start / end at the playhead"},
            {"Right-click the selection", "Clear in and out"},
            {"[Enter]", "Save the selection as a clip"},
            {"[+] / [-]", "Zoom in / out"},
            {"[Home]", "Follow the recording"},
        };
        // The hotkeys set in Options, their keys as caps: "Ctrl+Alt+S" becomes [Ctrl]+[Alt]+[S]
        static char hotkey_keys[COUNT(g_app.config.hotkeys)][96];
        static HelpRow hotkeys[COUNT(g_app.config.hotkeys)];
        i32 hk = 0;
        for (; hk < g_app.config.hotkey_count && hk < COUNT(hotkeys); hk++) {
            char *o = hotkey_keys[hk];
            i32 n = 0, cap = (i32)sizeof hotkey_keys[hk];
            o[n++] = '[';
            for (const char *k = g_app.config.hotkeys[hk].keys.s; *k && n < cap - 4; k++) {
                if (*k == '+') {
                    o[n++] = ']';
                    o[n++] = '+';
                    o[n++] = '[';
                } else o[n++] = *k;
            }
            o[n++] = ']';
            o[n] = 0;
            hotkeys[hk].keys = o;
            hotkeys[hk].what = g_app.config.hotkeys[hk].action.s;
        }

        // Title, the search and close
        ImVec2 top = ImGui::GetCursorScreenPos();
        f32 inner_w = win_w - 2 * pad, fh = ImGui::GetFrameHeight(), search_w = fminf(kit_px(240), inner_w / 2);
        kit_heading("Keyboard and mouse");
        ImGui::SetCursorScreenPos(ImVec2(top.x + inner_w - kit_px(8) - fh - kit_px(8) - search_w, top.y - kit_px(6)));
        if (ImGui::IsWindowAppearing()) q[0] = 0;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
        kit_search("##help_search", q, sizeof q, "Search", search_w);
        ImGui::SetCursorScreenPos(ImVec2(top.x + inner_w - fh + kit_px(8), top.y - kit_px(6)));
        kit_next_tip("Close", "Esc");
        if (kit_icon_button(ICON_CLOSE, "##help_close")) open = false;
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + kit_px(46)));

        // The sections in reading order, down one column and on to the next once it holds its share of the rows.
        // Placed by their full size, so a search only takes rows out and never moves a section to another column.
        HelpSection all[] = {
            {"HOTKEYS", hotkeys, hk},
            {"ANYWHERE (STREAM DECK)", anywhere, COUNT(anywhere)},
            {"LIBRARY", library, COUNT(library)},
            {"PLAYER", player, COUNT(player)},
            {"WINDOW", window, COUNT(window)},
            {"TIMELINE", timeline, COUNT(timeline)},
            {"EDITOR", editor, COUNT(editor)},
        };
        i32 n = COUNT(all), col_of[COUNT(all)], total = 0;
        for (i32 i = 0; i < n; i++) total += all[i].count ? all[i].count + 3 : 0; // a heading is about three rows
        for (i32 i = 0, c = 0, load = 0; i < n; i++) {
            i32 size = all[i].count ? all[i].count + 3 : 0;
            if (load > 0 && c < cols - 1 && load + size > (total + cols - 1) / cols) {
                c++;
                load = 0;
            }
            col_of[i] = c;
            load += size;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##help_rows", ImVec2(inner_w + pad - kit_px(6), 0), ImGuiChildFlags_None);
        ImGui::PopStyleVar();
        i32 shown = 0;
        for (i32 c = 0; c < cols; c++) {
            if (c) ImGui::SameLine(0, col_gap);
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(col_w, 0));
            for (i32 i = 0; i < n; i++)
                if (col_of[i] == c && all[i].count) {
                    for (i32 r = 0; r < all[i].count; r++) shown += help_row_shown(all[i].title, all[i].rows[r], q);
                    help_section(all[i].title, all[i].rows, all[i].count, col_w, q);
                }
            ImGui::EndGroup();
        }
        if (!shown) {
            ImGui::SetCursorPos(ImVec2(0, kit_px(8)));
            kit_dim("Nothing matches \"%s\"", q);
        }
        ImGui::EndChild();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_F1, false)) open = false;
        if (!open || g_app.ui.help_close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    g_app.ui.help_open = g_app.ui.help_close = false;
}

// A tab in the top bar: a label, white with an accent underline when it's the current screen.
static bool top_tab(const char *icon, const char *label, bool active, f32 h)
{
    ImGui::PushFont(g_font_bold);
    ImVec2 ts = ImGui::CalcTextSize(label);
    f32 slot = g_font_icon->FontSize + kit_px(8); // the icon, then the label
    f32 padx = kit_px(12), w = slot + ts.x + 2 * padx;
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    if (clicked) dev_click("tab", label);
    bool hot = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot && !active) dl->AddRectFilled(ImVec2(a.x, a.y + kit_px(7)), ImVec2(a.x + w, a.y + h - kit_px(7)), col32(COL_PANEL), kit_px(7));
    u32 ink = col32(active ? COL_TEXT : hot ? 0xd4d7dc : COL_DIM);
    dl->AddText(g_font_icon, g_font_icon->FontSize, ImVec2(a.x + padx, floorf(a.y + (h - g_font_icon->FontSize) / 2)),
                active ? col32(COL_ACCENT) : ink, icon);
    dl->AddText(ImVec2(a.x + padx + slot, floorf(a.y + (h - ts.y) / 2)), ink, label);
    if (active) dl->AddRectFilled(ImVec2(a.x + padx, a.y + h - kit_px(3)), ImVec2(a.x + w - padx, a.y + h), col32(COL_ACCENT), kit_px(2));
    ImGui::PopFont();
    return clicked;
}

// The bar across the top: the three screens as tabs, and on the right the recording switch with what the
// recorder is doing, then Help.
static void top_bar(f32 h)
{
    Recorder &rec = g_app.recorder;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    f32 w = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), col32(COL_SIDEBAR));
    dl->AddRectFilled(ImVec2(o.x, o.y + h - kit_px(1)), ImVec2(o.x + w, o.y + h), col32(COL_LINE));

    ImGui::SetCursorScreenPos(ImVec2(o.x + kit_px(12), o.y));
    Screen s = g_app.ui.screen;
    if (top_tab(ICON_LIBRARY, "Library", s == SCREEN_LIBRARY || s == SCREEN_EDITOR, h)) g_app.ui.screen = SCREEN_LIBRARY;
    ImGui::SameLine(0, kit_px(4));
    if (top_tab(ICON_TIMELINE, "Timeline", s == SCREEN_TIMELINE, h)) g_app.ui.screen = SCREEN_TIMELINE;
    ImGui::SameLine(0, kit_px(4));
    if (top_tab(ICON_SETTINGS, "Options", s == SCREEN_OPTIONS, h)) g_app.ui.screen = SCREEN_OPTIONS;

    // Right: recording on or off as one switch, what it's doing just before it, then Help. The switch and Help stay
    // put; the words end where the switch starts.
    f32 fh = ImGui::GetFrameHeight(), y = o.y + (h - fh) / 2;
    f32 help_x = o.x + w - kit_px(12) - fh, sw_w = kit_px(36), sw_x = help_x - kit_px(14) - sw_w;
    ImGui::SetCursorScreenPos(ImVec2(sw_x, y));
    bool on = !rec.paused;
    if (kit_toggle("##recording", &on)) event_post_text(EV_COMMAND, "toggle");
    kit_tip(rec.paused ? "Start recording" : "Stop recording (nothing is kept while off)");
    ImGui::SetCursorScreenPos(ImVec2(help_x, y));
    kit_next_tip("Keyboard and mouse", "F1");
    if (kit_icon_button(ICON_HELP, "##help", KIT_GHOST)) g_app.ui.help_open = true;

    const char *status = rec.paused ? "Off" : rec.recording ? "Recording" : rec.last_error.s[0] ? "Not recording - see Options"
                                                                                               : "Starting";
    ImGui::PushFont(g_font_bold);
    ImVec2 ss = ImGui::CalcTextSize(status);
    f32 tx = sw_x - kit_px(10) - ss.x;
    u32 dot = rec.paused ? COL_FAINT : rec.recording ? COL_RED : rec.last_error.s[0] ? COL_WARN : COL_FAINT;
    ImVec2 dc(tx - kit_px(12), o.y + h / 2);
    dl->AddCircleFilled(dc, kit_px(4.5f), col32(dot));
    if (rec.recording && !rec.paused) dl->AddCircle(dc, kit_px(8), col32(COL_RED, 0.35f), 0, kit_px(1.5f));
    dl->AddText(ImVec2(tx, floorf(o.y + (h - ss.y) / 2)), col32(rec.paused ? COL_DIM : COL_TEXT), status);
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, h));
}

// Dev mode: keys pressed, mouse clicks (where, and over which window and item), and screen changes.
static void dev_input()
{
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiIO &io = ImGui::GetIO();
    for (i32 k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
        ImGuiKey key = (ImGuiKey)k;
        if (ImGui::IsMouseKey(key) || ImGui::IsLRModKey(key) || ImGui::IsAliasKey(key) || (k >= ImGuiKey_ReservedForModCtrl && k <= ImGuiKey_ReservedForModSuper)) continue;
        if (!ImGui::IsKeyPressed(key, false)) continue;
        log_line("%s: key %s%s%s%s", screen_name(g_app.ui.screen), io.KeyCtrl ? "Ctrl+" : "", io.KeyShift ? "Shift+" : "",
                 io.KeyAlt ? "Alt+" : "", ImGui::GetKeyName(key));
    }
    static const char *const buttons[] = {"left", "right", "middle", "x1", "x2"};
    for (i32 b = 0; b < 5; b++) {
        if (!ImGui::IsMouseClicked(b)) continue;
        log_line("%s: %s click at %.0f,%.0f over \"%s\" (item %08x)%s", screen_name(g_app.ui.screen), buttons[b],
                 io.MousePos.x, io.MousePos.y, g.HoveredWindow ? g.HoveredWindow->Name : "nothing",
                 g.HoveredIdPreviousFrame, ImGui::IsMouseDoubleClicked(b) ? ", double" : "");
    }
    static i32 last_screen = -1;
    if (last_screen != (i32)g_app.ui.screen) {
        if (last_screen >= 0) log_line("screen %s -> %s", screen_name((Screen)last_screen), screen_name(g_app.ui.screen));
        last_screen = (i32)g_app.ui.screen;
    }
}

static void ui_frame()
{
    {
        FRAME_PART("new frame");
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }
    ui_popups_click_through();
    ImGuiContext &g = *ImGui::GetCurrentContext();
    g.DebugLogFlags = ImGuiDebugLogFlags_EventError | ImGuiDebugLogFlags_OutputToTTY |
                      (g_dev ? ImGuiDebugLogFlags_EventPopup | ImGuiDebugLogFlags_EventFocus : 0);
    ImGui::GetIO().ConfigDebugHighlightIdConflicts = g_dev; // ImGui's ID conflict box is for the detailed log only
    if (g_dev) dev_input();

    if (ImGui::IsKeyPressed(ImGuiKey_F1, false) && !ImGui::IsPopupOpen("Help")) {
        g_app.ui.help_open = true;
        ui_wake(6); // a modal takes a few frames to open and size itself; don't wait for the mouse to move
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("main", 0,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    if (g_app.ui.setup_open) {
        setup_window();
        help_window();
        ImGui::End();
        goto rendered;
    }
    top_bar(kit_px(46));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
    // The screen: padded content area under the top bar. Screens lay out inside it with GetContentRegionAvail.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kit_px(24), kit_px(16)));
    ImGui::BeginChild("##screen", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    screens_left_behind(); // editor.cpp: closes the player of a screen that isn't shown any more
    switch (g_app.ui.screen) { // each screen's layout and drawing, with what it uploads and updates
    case SCREEN_LIBRARY: { FRAME_PART("library screen"); screen_library(); } break;
    case SCREEN_TIMELINE: { FRAME_PART("timeline screen"); screen_timeline(); } break;
    case SCREEN_EDITOR: { FRAME_PART("editor screen"); screen_editor(); } break;
    case SCREEN_OPTIONS: { FRAME_PART("options screen"); screen_options(); } break;
    }
    library_hover_end_frame(); // the Library cards' preview stops once no card has the mouse
    ImGui::EndChild();
    help_window();
    ImGui::End();

rendered:
    {
        FRAME_PART("render");
        ImGui::Render();
        const ImVec4 bg = rgb(COL_BG);
        const f32 clear[4] = {bg.x, bg.y, bg.z, 1};
        g_gpu.context->OMSetRenderTargets(1, &g_gpu.target, 0);
        g_gpu.context->ClearRenderTargetView(g_gpu.target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    {
        FRAME_PART("present");
        g_gpu.swapchain->Present(1, 0);
    }
    g.DebugLogBuf.clear(); // already in our log (imgui_debug_print); don't let it grow
    g.DebugLogIndex.clear();
}
