// clipback: the whole program, built as one translation unit (build.bat compiles only this file).
// See SPEC.md for what it does and how it's put together.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <dbghelp.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <wincodec.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <signal.h>

// A failed ImGui assert would show a message box and abort with nothing in the log; log it and crash instead,
// so the crash handler writes a minidump (main.cpp).
static void imgui_assert_failed(const char *expr, const char *file, int line);
#define IM_ASSERT(e) ((e) ? (void)0 : imgui_assert_failed(#e, __FILE__, __LINE__))

#include "vendor/imgui/imgui.cpp"
#include "vendor/imgui/imgui_draw.cpp"
#include "vendor/imgui/imgui_tables.cpp"
#include "vendor/imgui/imgui_widgets.cpp"
#include "vendor/imgui/imgui_impl_win32.cpp"
#include "vendor/imgui/imgui_impl_dx11.cpp"

#include "base.h"
#include "app.h"

static void imgui_assert_failed(const char *expr, const char *file, int line)
{
    log_line("ImGui assert failed at %s:%d: %s", file, line, expr);
    __debugbreak();
}
#include "events.cpp"
#include "config.cpp"
#include "proc.cpp"
#include "recorder.cpp"
#include "audio.cpp"
#include "buffer.cpp"
#include "save.cpp"
#include "hotkeys.cpp"
#include "tray.cpp"
#include "ui.cpp"
#include "main.cpp"
