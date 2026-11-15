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

// Colours, from the dark theme the current window uses.
static ImVec4 rgb(u32 c, f32 a = 1) { return ImVec4(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a); }
static const u32 COL_BG = 0x111317, COL_PANEL = 0x1a1d23, COL_PANEL2 = 0x22262e, COL_LINE = 0x2e333d,
                 COL_TEXT = 0xe8eaee, COL_DIM = 0x9097a3, COL_ACCENT = 0xe8333f, COL_BLUE = 0x4d8dff,
                 COL_HOVER = 0x2b3039, COL_OK = 0x3fbf74;

static ImFont *g_font, *g_font_bold, *g_font_big;
static f32 g_dpi_scale = 1;

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
    HRESULT hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
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

    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0x25CF, 0x25CF, 0}; // Latin-1, dashes, arrows, ●
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    g_font = io.Fonts->AddFontFromFileTTF(regular, 15 * scale, &cfg, ranges);
    g_font_bold = io.Fonts->AddFontFromFileTTF(semibold, 15 * scale, &cfg, ranges);
    g_font_big = io.Fonts->AddFontFromFileTTF(semibold, 20 * scale, &cfg, ranges);
    if (!g_font) g_font = g_font_bold = g_font_big = io.Fonts->AddFontDefault();
    io.Fonts->Build();
    ImGui_ImplDX11_InvalidateDeviceObjects(); // the font texture is remade on the next frame
}

static void ui_style(f32 scale)
{
    ImGuiStyle &s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 0;
    s.FrameRounding = 6;
    s.ChildRounding = 8;
    s.PopupRounding = 6;
    s.GrabRounding = 6;
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 8);
    s.WindowPadding = ImVec2(16, 12);
    s.WindowBorderSize = 0;
    s.FrameBorderSize = 1;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = rgb(COL_BG);
    c[ImGuiCol_ChildBg] = rgb(COL_PANEL);
    c[ImGuiCol_PopupBg] = rgb(COL_PANEL);
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
    c[ImGuiCol_HeaderActive] = rgb(COL_LINE);
    c[ImGuiCol_CheckMark] = rgb(COL_BLUE);
    c[ImGuiCol_SliderGrab] = rgb(COL_BLUE);
    c[ImGuiCol_SliderGrabActive] = rgb(COL_BLUE);
    c[ImGuiCol_Separator] = rgb(COL_LINE);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    c[ImGuiCol_ScrollbarBg] = rgb(COL_BG);
    c[ImGuiCol_ScrollbarGrab] = rgb(COL_LINE);
    s.ScaleAllSizes(scale);
}

static void ui_set_scale(f32 scale)
{
    g_dpi_scale = scale;
    ui_load_fonts(scale);
    ui_style(scale);
}

static LRESULT CALLBACK ui_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
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
        ShowWindow(hwnd, SW_HIDE);
        g_app.ui.shown = false;
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
    wc.lpfnWndProc = ui_wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(0, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0x11, 0x13, 0x17));
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
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_gpu.device, g_gpu.context);
    ui_set_scale(GetDpiForWindow(hwnd) / 96.f);
    return true;
}

static void ui_show(Screen screen)
{
    Ui &ui = g_app.ui;
    ui.screen = screen;
    if (!ui.shown) {
        ShowWindow(ui.hwnd, IsZoomed(ui.hwnd) ? SW_SHOWMAXIMIZED : SW_SHOW);
        ui.shown = true;
    }
    if (IsIconic(ui.hwnd)) ShowWindow(ui.hwnd, SW_RESTORE);
    // A second launch (Stream Deck, hotkey) gave us the right to take the foreground; the tray did too.
    SetForegroundWindow(ui.hwnd);
    ui_wake(4);
}

// ---------------------------------------------------------------------------------------------------------------
// Screens. Each module's screen fills these in; for now they're placeholders.

static void screen_library() { ImGui::TextDisabled("Library: clips and sessions."); }
static void screen_timeline() { ImGui::TextDisabled("Timeline: the buffer."); }
static void screen_editor() { ImGui::TextDisabled("Editor."); }
static void screen_options() { ImGui::TextDisabled("Options."); }

struct HelpRow { const char *keys, *what; };
struct HelpSection { const char *title; const HelpRow *rows; i32 count; };

static void help_section(const char *title, const HelpRow *rows, i32 count)
{
    ImGui::PushFont(g_font_bold);
    ImGui::TextDisabled("%s", title);
    ImGui::PopFont();
    for (i32 i = 0; i < count; i++) {
        ImGui::TextUnformatted(rows[i].keys);
        ImGui::SameLine(220 * g_dpi_scale);
        ImGui::TextUnformatted(rows[i].what);
    }
    ImGui::Spacing();
}

static void help_window()
{
    if (g_app.ui.help_open) ImGui::OpenPopup("Help");
    ImGui::SetNextWindowSize(ImVec2(640 * g_dpi_scale, 0));
    bool open = true;
    if (ImGui::BeginPopupModal("Help", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        static const HelpRow anywhere[] = {
            {"clipback save 90s", "Save the last 90 s (or 30s, 2m, 1h …)"},
            {"clipback timeline", "Open the buffer timeline"},
            {"clipback open", "Open this window"},
            {"clipback pause", "Pause / resume recording"},
        };
        static const HelpRow window[] = {{"F1", "This help"}, {"Esc", "Close this help"}};
        ImGui::PushFont(g_font_bold);
        ImGui::TextDisabled("HOTKEYS");
        ImGui::PopFont();
        for (i32 i = 0; i < g_app.config.hotkey_count; i++) {
            ImGui::TextUnformatted(g_app.config.hotkeys[i].keys.s);
            ImGui::SameLine(220 * g_dpi_scale);
            ImGui::TextUnformatted(g_app.config.hotkeys[i].action.s);
        }
        ImGui::Spacing();
        help_section("ANYWHERE (STREAM DECK)", anywhere, COUNT(anywhere));
        help_section("WINDOW", window, COUNT(window));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_F1, false)) open = false;
        if (!open) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    g_app.ui.help_open = false;
}

static bool header_tab(const char *label, Screen screen)
{
    bool active = g_app.ui.screen == screen;
    ImGui::PushStyleColor(ImGuiCol_Button, active ? rgb(COL_PANEL2) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, active ? rgb(COL_TEXT) : rgb(COL_DIM));
    bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(2);
    if (clicked) g_app.ui.screen = screen;
    return clicked;
}

static void header()
{
    Recorder &rec = g_app.recorder;
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted("clipback");
    ImGui::PopFont();
    ImGui::SameLine(0, 24 * g_dpi_scale);
    header_tab("Library", SCREEN_LIBRARY);
    ImGui::SameLine();
    header_tab("Timeline", SCREEN_TIMELINE);
    ImGui::SameLine();
    header_tab("Options", SCREEN_OPTIONS);

    // Right side, laid out from fixed widths so nothing moves when the status text changes
    f32 help_w = 70 * g_dpi_scale, pause_w = 90 * g_dpi_scale, status_w = 220 * g_dpi_scale;
    f32 right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine(right - help_w - pause_w - status_w - 2 * ImGui::GetStyle().ItemSpacing.x);
    const char *status = rec.paused ? "Paused" : rec.recording ? "Recording" : "Not recording";
    ImVec4 dot = rec.paused ? rgb(COL_DIM) : rec.recording ? rgb(COL_ACCENT) : rgb(COL_DIM);
    ImGui::PushStyleColor(ImGuiCol_Text, dot);
    ImGui::TextUnformatted("\xE2\x97\x8F"); // ●
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(status);
    ImGui::SameLine(right - help_w - pause_w - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(rec.paused ? "Resume" : "Pause", ImVec2(pause_w, 0))) event_post_text(EV_COMMAND, "toggle");
    ImGui::SameLine(right - help_w);
    if (ImGui::Button("Help", ImVec2(help_w, 0))) g_app.ui.help_open = true;
    ImGui::Separator();
}

static void ui_frame()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (ImGui::IsKeyPressed(ImGuiKey_F1, false) && !ImGui::IsPopupOpen("Help")) g_app.ui.help_open = true;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("main", 0,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    header();
    switch (g_app.ui.screen) {
    case SCREEN_LIBRARY: screen_library(); break;
    case SCREEN_TIMELINE: screen_timeline(); break;
    case SCREEN_EDITOR: screen_editor(); break;
    case SCREEN_OPTIONS: screen_options(); break;
    }
    help_window();
    ImGui::End();

    ImGui::Render();
    const ImVec4 bg = rgb(COL_BG);
    const f32 clear[4] = {bg.x, bg.y, bg.z, 1};
    g_gpu.context->OMSetRenderTargets(1, &g_gpu.target, 0);
    g_gpu.context->ClearRenderTargetView(g_gpu.target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_gpu.swapchain->Present(1, 0);
}
