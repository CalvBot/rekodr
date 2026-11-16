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
#define ICON_CUT      "\xEE\xA3\x86" // E8C6
#define ICON_MORE     "\xEE\x9C\x92" // E712
#define ICON_COPY     "\xEE\xA3\x88" // E8C8
#define ICON_CLOCK    "\xEE\xA0\xA3" // E823
#define ICON_GAME     "\xEE\x9F\xBC" // E7FC
#define ICON_MIC      "\xEE\x9C\xA0" // E720
#define ICON_CHECK    "\xEE\x9C\xBE" // E73E
#define ICON_KEYBOARD "\xEE\x9D\xA5" // E765
#define ICON_STORAGE  "\xEE\xB6\xA2" // EDA2 hard drive
#define ICON_VIDEO    "\xEE\x9C\x94" // E714
#define ICON_POWER    "\xEE\x9F\xA8" // E7E8
#define ICON_WARNING  "\xEE\x9E\xBA" // E7BA
#define ICON_NOW      "\xEE\xA2\x93" // E893 skip to end
#define ICON_FULLSCREEN "\xEE\x9D\x80" // E740
#define ICON_APPS     "\xEE\x9C\x9D" // E71D all apps
#define ICON_SLIDERS  "\xEE\xA7\xA9" // E9E9 equalizer
#define ICON_MONITOR   "\xEE\x9F\xB4" // E7F4
#define ICON_ASPECT    "\xEE\x9E\x99" // E799
#define ICON_STOPWATCH "\xEE\xA4\x96" // E916
#define ICON_MOUSE     "\xEE\xA5\xA2" // E962
#define ICON_SPEED     "\xEE\xB1\x8A" // EC4A
#define ICON_HOME      "\xEE\xA0\x8F" // E80F
#define ICON_CHIP      "\xEE\xA5\x90" // E950
#define ICON_PAGE      "\xEE\x9F\x83" // E7C3
#define ICON_STAR      "\xEE\x9C\xB4" // E734
#define ICON_UP        "\xEE\xA2\x98" // E898
#define ICON_COLOR     "\xEE\x9E\x90" // E790
#define ICON_FILTER    "\xEE\x9C\x9C" // E71C
#define ICON_LOCK      "\xEE\x9C\xAE" // E72E
#define ICON_HISTORY   "\xEE\xA0\x9C" // E81C
#define ICON_AUDIO     "\xEE\xA3\x96" // E8D6
#define ICON_FILE      "\xEE\xA3\xA5" // E8E5
#define ICON_BRUSH     "\xEE\x9D\xB1" // E771
#define ICON_PICTURE   "\xEE\xA2\xB9" // E8B9
#define ICON_UNDO      "\xEE\x9E\xA7" // E7A7
#define ICON_SORT      "\xEE\xA3\x8B" // E8CB
#define ICON_WINDOW    "\xEE\x9C\xB7" // E737
#define ICON_CHAT      "\xEE\xA2\xBD" // E8BD
#define ICON_START     "\xEE\xA2\x92" // E892
#define ICON_CALENDAR "\xEE\x9E\x87" // E787
#define ICON_BUG      "\xEE\xAF\xA8" // EBE8
#define ICON_LIST      "\xEE\xA3\xBD" // E8FD

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

    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0x25CF, 0x25CF, 0}; // Latin-1, dashes, arrows, ●
    static const ImWchar icon_ranges[] = {0xE700, 0xF8FF, 0};
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    ImFontConfig merge;
    merge.MergeMode = true;
    merge.OversampleH = 2;
    merge.PixelSnapH = true;
    merge.GlyphOffset = ImVec2(0, 2 * scale); // the icons sit high next to Segoe UI's x-height
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

static LRESULT ui_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        ui_wake();
        return 1;
    }
    switch (msg) {
    case WM_SIZE:
        gpu_resize(LOWORD(lp), HIWORD(lp));
        ui_wake();
        return 0;
    case WM_CLOSE:
        DEV_LOG("window closed (hidden)");
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
    case WM_GETMINMAXINFO: {
        MINMAXINFO *m = (MINMAXINFO *)lp;
        m->ptMinTrackSize.x = (LONG)(900 * g_dpi_scale);
        m->ptMinTrackSize.y = (LONG)(600 * g_dpi_scale);
        return 0;
    }
    }
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ui_wake();
    if (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ui_wake();
    return DefWindowProcW(hwnd, msg, wp, lp);
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

    // Centered on the main monitor, sized for it
    f32 scale = GetDpiForSystem() / 96.f;
    i32 w = (i32)(1280 * scale), h = (i32)(800 * scale);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
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
    if (!ui.shown) {
        ShowWindow(ui.hwnd, IsZoomed(ui.hwnd) ? SW_SHOWMAXIMIZED : SW_SHOW);
        ui.shown = true;
        library_window_shown(true);
    }
    if (IsIconic(ui.hwnd)) ShowWindow(ui.hwnd, SW_RESTORE);
    // A second launch (Stream Deck, hotkey) gave us the right to take the foreground; the tray did too.
    SetForegroundWindow(ui.hwnd);
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
    ImGui::PushStyleColor(ImGuiCol_Button, rgb(bg, bg_a));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(hover, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(text));
    if (kind == KIT_PRIMARY) ImGui::PushFont(g_font_bold);
    if (!enabled) ImGui::BeginDisabled();
    bool clicked = ImGui::Button(label, size);
    if (!enabled) ImGui::EndDisabled();
    if (clicked && enabled) dev_click("button", label);
    if (kind == KIT_PRIMARY) ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return clicked && enabled;
}

// A square button showing one icon (ICON_*). id must be unique in the window ("##play"). active draws it selected.
static bool kit_icon_button(const char *icon, const char *id, KitKind kind = KIT_GHOST, bool enabled = true,
                            bool active = false, f32 size = 0)
{
    if (size <= 0) size = ImGui::GetFrameHeight();
    ImVec2 a = ImGui::GetCursorScreenPos();
    if (!enabled) ImGui::BeginDisabled();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    if (!enabled) ImGui::EndDisabled();
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
    ImGui::PushFont(g_font_icon);
    ImVec2 ts = ImGui::CalcTextSize(ICON_SEARCH);
    ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + kit_px(12), floorf(a.y + (ImGui::GetFrameHeight() - ts.y) / 2) + kit_px(1)),
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
static void kit_title(const char *text)
{
    ImGui::PushFont(g_font_title);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}
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
static void kit_dim_wrapped(const char *text) // one short sentence of explanation; never a paragraph
{
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_DIM));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Places the next item so it ends at the right edge of the content region (w = its width).
static void kit_align_right(f32 w)
{
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - w);
}

// ---------------------------------------------------------------------------------------------------------------
// Screens, each in its own file. They can use everything above (theme colours, fonts, ui_wake, the kit).

#include "sessions.cpp"
#include "player.cpp"
#include "library.cpp"
#include "editor.cpp"
#include "timeline.cpp"
#include "options.cpp"

struct HelpRow { const char *keys, *what; };
struct HelpSection { const char *title; const HelpRow *rows; i32 count; };

// One group of the Help window, in a column w wide. A row's keys are written with markup: [Ctrl] is drawn as a key
// cap, `clipback open` as a command, and anything else (mouse actions, "+", " / ") as plain text.
static void help_section(const char *title, const HelpRow *rows, i32 count, f32 w)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 keys_w = kit_px(168), gap = kit_px(8);
    kit_label(title);
    ImGui::Dummy(ImVec2(0, kit_px(2)));
    for (i32 i = 0; i < count; i++) {
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
                    dl->AddRectFilled(ImVec2(x, y + kit_px(2)), ImVec2(x + cw, y + cap_h + kit_px(2)), col32(0x0c0d10), kit_px(5));
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw, y + cap_h), col32(COL_HOVER), kit_px(5));
                    dl->AddText(ImVec2(floorf(x + (cw - ts.x) / 2), floorf(y + (cap_h - sh) / 2)), col32(COL_TEXT), p + 1, end);
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
    f32 col_w = kit_px(372), col_gap = kit_px(40), pad = kit_px(28);
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y / 2), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(2 * col_w + col_gap + 2 * pad, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, kit_px(24)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kit_px(14));
    bool open = true;
    if (ImGui::BeginPopupModal("Help", &open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        static const HelpRow anywhere[] = {
            {"`clipback save 90s`", "Save the last 90 s (or 30s, 2m, 1h \xE2\x80\xA6)"},
            {"`clipback timeline`", "Open the buffer timeline"},
            {"`clipback open`", "Open this window"},
            {"`clipback pause`", "Pause / resume recording"},
        };
        static const HelpRow window[] = {{"[F1]", "This help"}, {"[Esc]", "Close this help"},
                                         {"[Esc]", "Options, setting a hotkey's keys: cancel"}};
        static const HelpRow library[] = {
            {"[Ctrl]+[F]", "Search"},
            {"[Page Up] / [Page Down]", "Previous / next page (or the mouse wheel)"},
            {"Point at a thumbnail", "Scrub through it; click to play from there"},
            {"Right-click a card", "Edit, rename, delete and more"},
        };
        static const HelpRow player[] = {
            {"[Space]", "Play / pause"},
            {"[\xE2\x86\x90] / [\xE2\x86\x92]", "Back / forward 5 s"},
            {"[Esc]", "Close the player"},
        };
        static const HelpRow editor[] = {
            {"[Space]", "Play / pause"},
            {"[I] / [O]", "Trim start / end at the playhead"},
            {"[K]", "Zoom keyframe at the playhead"},
            {"[Delete]", "Delete the keyframe at the playhead"},
            {"[\xE2\x86\x90] / [\xE2\x86\x92]", "Step one frame"},
            {"[Shift]+[\xE2\x86\x90] / [\xE2\x86\x92]", "Step 1 s"},
            {"Drag the picture", "Aim the zoom (when zoomed in)"},
            {"Double-click a volume", "Back to as recorded"},
            {"[Esc]", "Back to the library"},
        };
        static const HelpRow timeline[] = {
            {"Drag across the tracks", "Select a range (drag its edges to adjust)"},
            {"Click on the tracks", "Move the playhead"},
            {"[Space]", "Play or pause from the playhead"},
            {"Mouse wheel", "Zoom in and out around the mouse"},
            {"Wheel over the names", "Scroll the sound tracks"},
            {"Right or middle drag", "Pan earlier or later"},
            {"[\xE2\x86\x90] / [\xE2\x86\x92]", "Move the selection 1 s (with Shift 10 s)"},
            {"[I] / [O]", "Set the start / end at the mouse, or the playhead"},
            {"[Enter]", "Save the selection as a clip"},
            {"[+] / [-]", "Zoom in / out"},
            {"[Home]", "Back to now"},
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

        // Title and close
        ImVec2 top = ImGui::GetCursorScreenPos();
        kit_heading("Keyboard and mouse");
        ImGui::SetCursorScreenPos(ImVec2(top.x + 2 * col_w + col_gap - ImGui::GetFrameHeight() + kit_px(8), top.y - kit_px(6)));
        if (kit_icon_button(ICON_CLOSE, "##help_close")) open = false;
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + kit_px(46)));

        ImGui::BeginGroup();
        if (hk) help_section("HOTKEYS", hotkeys, hk, col_w);
        help_section("ANYWHERE (STREAM DECK)", anywhere, COUNT(anywhere), col_w);
        help_section("LIBRARY", library, COUNT(library), col_w);
        help_section("PLAYER", player, COUNT(player), col_w);
        help_section("WINDOW", window, COUNT(window), col_w);
        ImGui::EndGroup();
        ImGui::SameLine(0, col_gap);
        ImGui::BeginGroup();
        help_section("TIMELINE", timeline, COUNT(timeline), col_w);
        help_section("EDITOR", editor, COUNT(editor), col_w);
        ImGui::EndGroup();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_F1, false)) open = false;
        if (!open) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    g_app.ui.help_open = false;
}

// A tab in the top bar: a label, white with an accent underline when it's the current screen.
static bool top_tab(const char *icon, const char *label, bool active, f32 h)
{
    ImGui::PushFont(g_font_bold);
    ImVec2 ts = ImGui::CalcTextSize(label);
    f32 slot = g_font_icon->FontSize + kit_px(8); // the icon, then the label
    f32 padx = kit_px(16), w = slot + ts.x + 2 * padx;
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    if (clicked) dev_click("tab", label);
    bool hot = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot && !active) dl->AddRectFilled(ImVec2(a.x, a.y + kit_px(10)), ImVec2(a.x + w, a.y + h - kit_px(10)), col32(COL_PANEL), kit_px(7));
    u32 ink = col32(active ? COL_TEXT : hot ? 0xd4d7dc : COL_DIM);
    dl->AddText(g_font_icon, g_font_icon->FontSize, ImVec2(a.x + padx, floorf(a.y + (h - g_font_icon->FontSize) / 2)),
                active ? col32(COL_ACCENT) : ink, icon);
    dl->AddText(ImVec2(a.x + padx + slot, floorf(a.y + (h - ts.y) / 2)), ink, label);
    if (active) dl->AddRectFilled(ImVec2(a.x + padx, a.y + h - kit_px(3)), ImVec2(a.x + w - padx, a.y + h), col32(COL_ACCENT), kit_px(2));
    ImGui::PopFont();
    return clicked;
}

// The bar across the top: the three screens as tabs, and on the right what the recorder is doing with
// its Pause / Resume, then Help. The button and Help stay put whatever the status says.
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

    // Right: the status, Pause / Resume, Help.
    f32 fh = ImGui::GetFrameHeight(), y = o.y + (h - fh) / 2;
    f32 help_x = o.x + w - kit_px(16) - fh;
    const char *btn = rec.paused ? ICON_RECORD "  Resume" : ICON_PAUSE "  Pause";
    f32 btn_w = kit_px(104), btn_x = help_x - kit_px(8) - btn_w;
    ImGui::SetCursorScreenPos(ImVec2(btn_x, y));
    if (kit_button(btn, KIT_SECONDARY, ImVec2(btn_w, 0))) event_post_text(EV_COMMAND, "toggle");
    ImGui::SetCursorScreenPos(ImVec2(help_x, y));
    if (kit_icon_button(ICON_HELP, "##help", KIT_GHOST)) g_app.ui.help_open = true;

    const char *status = rec.paused ? "Paused" : rec.recording ? "Recording" : "Not recording";
    char detail[64] = "";
    if (rec.paused) str_set(detail, "nothing kept");
    else if (rec.recording && rec.recording_since_us) {
        SYSTEMTIME t = local_time(rec.recording_since_us);
        snprintf(detail, sizeof detail, "since %d:%02d", t.wHour, t.wMinute);
    } else if (rec.last_error.s[0]) str_set(detail, "see Options");
    else str_set(detail, "starting");
    ImGui::PushFont(g_font_bold);
    ImVec2 ss = ImGui::CalcTextSize(status);
    ImGui::PopFont();
    ImVec2 ds = ImGui::CalcTextSize(detail);
    f32 gap = kit_px(8), tx = btn_x - kit_px(16) - ds.x - gap - ss.x;
    f32 ty = floorf(o.y + (h - ss.y) / 2);
    u32 dot = rec.paused ? COL_WARN : rec.recording ? COL_RED : COL_FAINT;
    ImVec2 dc(tx - kit_px(14), o.y + h / 2);
    dl->AddCircleFilled(dc, kit_px(4.5f), col32(dot));
    if (rec.recording && !rec.paused) dl->AddCircle(dc, kit_px(8), col32(COL_RED, 0.35f), 0, kit_px(1.5f));
    ImGui::PushFont(g_font_bold);
    dl->AddText(ImVec2(tx, ty), col32(COL_TEXT), status);
    ImGui::PopFont();
    dl->AddText(ImVec2(tx + ss.x + gap, floorf(o.y + (h - ds.y) / 2)), col32(COL_DIM), detail);

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
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGuiContext &g = *ImGui::GetCurrentContext();
    g.DebugLogFlags = ImGuiDebugLogFlags_EventError | ImGuiDebugLogFlags_OutputToTTY |
                      (g_dev ? ImGuiDebugLogFlags_EventPopup | ImGuiDebugLogFlags_EventFocus : 0);
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
    top_bar(kit_px(56));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
    // The screen: padded content area under the top bar. Screens lay out inside it with GetContentRegionAvail.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kit_px(28), kit_px(24)));
    ImGui::BeginChild("##screen", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    screens_left_behind(); // editor.cpp: closes the player of a screen that isn't shown any more
    switch (g_app.ui.screen) {
    case SCREEN_LIBRARY: screen_library(); break;
    case SCREEN_TIMELINE: screen_timeline(); break;
    case SCREEN_EDITOR: screen_editor(); break;
    case SCREEN_OPTIONS: screen_options(); break;
    }
    ImGui::EndChild();
    help_window();
    sessions_popup(); // "Save that session?" when a session has just ended
    ImGui::End();

    ImGui::Render();
    const ImVec4 bg = rgb(COL_BG);
    const f32 clear[4] = {bg.x, bg.y, bg.z, 1};
    g_gpu.context->OMSetRenderTargets(1, &g_gpu.target, 0);
    g_gpu.context->ClearRenderTargetView(g_gpu.target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_gpu.swapchain->Present(1, 0);
    g.DebugLogBuf.clear(); // already in our log (imgui_debug_print); don't let it grow
    g.DebugLogIndex.clear();
}
