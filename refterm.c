#include <windows.h>
#include <shlwapi.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <stddef.h>
#include <stdint.h>
#include <intrin.h>
#include <usp10.h>
#include <strsafe.h>
#include <stdarg.h>

#include <intrin.h>

#include "refterm.h"

#include "refterm_glyph_cache.h"
#include "refterm_glyph_cache.c"

#include "refterm_vs.h"
#include "refterm_ps.h"
#include "refterm_cs.h"
#include "refterm_example_source_buffer.h"
#include "refterm_example_dwrite.h"
#include "refterm_example_d3d11.h"
#include "refterm_example_glyph_generator.h"
#include "refterm_example_terminal.h"
#include "refterm_example_source_buffer.c"
#include "refterm_example_glyph_generator.c"
#include "refterm_example_d3d11.c"
#include "refterm_example_terminal.c"

#pragma comment (lib, "kernel32")
#pragma comment (lib, "user32")
#pragma comment (lib, "gdi32")
#pragma comment (lib, "usp10")
#pragma comment (lib, "dwrite")
#pragma comment (lib, "d2d1")
#pragma comment (lib, "mincore")

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    LRESULT Result = 0;

    switch (Message)
    {
        case WM_CLOSE:
        case WM_DESTROY:
        {
            PostQuitMessage(0);
        } break;

        default:
        {
            Result = DefWindowProcW(Window, Message, WParam, LParam);
        } break;
    }

    return Result;
}

typedef BOOL WINAPI set_process_dpi_aware(void);
typedef BOOL WINAPI set_process_dpi_awareness_context(DPI_AWARENESS_CONTEXT);
static void PreventWindowsDPIScaling()
{
    HMODULE WinUser = LoadLibraryW(L"user32.dll");
    set_process_dpi_awareness_context *SetProcessDPIAwarenessContext = (set_process_dpi_awareness_context *)GetProcAddress(WinUser, "SetProcessDPIAwarenessContext");
    if(SetProcessDPIAwarenessContext)
    {
        SetProcessDPIAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    }
    else
    {
        set_process_dpi_aware *SetProcessDPIAware = (set_process_dpi_aware *)GetProcAddress(WinUser, "SetProcessDPIAware");
        if(SetProcessDPIAware)
        {
            SetProcessDPIAware();
        }
    }
}

static HWND CreateOutputWindow()
{
    WNDCLASSEXW WindowClass =
    {
        .cbSize = sizeof(WindowClass),
        .lpfnWndProc = &WindowProc,
        .hInstance = GetModuleHandleW(NULL),
        .hIcon = LoadIconA(NULL, IDI_APPLICATION),
        .hCursor = LoadCursorA(NULL, IDC_ARROW),
        .hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH),
        .lpszClassName = L"reftermclass",
    };

    HWND Result = {0};
    if(RegisterClassExW(&WindowClass))
    {
        // NOTE(casey): Martins says WS_EX_NOREDIRECTIONBITMAP is necessary to make
        // DXGI_SWAP_EFFECT_FLIP_DISCARD "not glitch on window resizing", and since
        // I don't normally program DirectX and have no idea, we're just going to
        // leave it here :)
        DWORD ExStyle = WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP;

        Result = CreateWindowExW(ExStyle, WindowClass.lpszClassName, L"refterm", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                 0, 0, WindowClass.hInstance, 0);
    }

    return Result;
}

void WinMainCRTStartup()
{
    PreventWindowsDPIScaling();

    HWND Window = CreateOutputWindow();
    Assert(IsWindow(Window));

    DWORD RenderThreadID;
    CreateThread(0, 0, TerminalThread, Window, 0, &RenderThreadID);

    for(;;)
    {
        MSG Message;
        GetMessageW(&Message, 0, 0, 0);
        TranslateMessage(&Message);
        if((Message.message == WM_CHAR) ||
           (Message.message == WM_KEYDOWN) ||
           (Message.message == WM_QUIT))
        {
            PostThreadMessage(RenderThreadID, Message.message, Message.wParam, Message.lParam);
        }
        else
        {
            DispatchMessageW(&Message);
        }
    }
}

// CRT stuff

int _fltused = 0x9875;

#pragma function(memset)
void *memset(void *DestInit, int Source, size_t Size)
{
    unsigned char *Dest = (unsigned char *)DestInit;
    while(Size--) *Dest++ = (unsigned char)Source;

    return(DestInit);
}

#pragma function(memcpy)
void *memcpy(void *DestInit, void const *SourceInit, size_t Size)
{
    unsigned char *Source = (unsigned char *)SourceInit;
    unsigned char *Dest = (unsigned char *)DestInit;
    while(Size--) *Dest++ = *Source++;

    return(DestInit);
}
