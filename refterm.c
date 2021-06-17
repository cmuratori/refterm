#define COBJMACROS
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shlwapi.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <stddef.h>
#include <stdint.h>
#include <intrin.h>

#include "refterm_shader.h"

#pragma comment (lib, "kernel32.lib")
#pragma comment (lib, "user32.lib")
#pragma comment (lib, "gdi32.lib")
#pragma comment (lib, "dxguid.lib")
#pragma comment (lib, "d3d11.lib")

#define Assert(cond) do { if (!(cond)) __debugbreak(); } while (0)
#define AssertHR(hr) Assert(SUCCEEDED(hr))

static HANDLE FrameLatencyWaitableObject;
static IDXGISwapChain2* SwapChain;
static ID3D11Device* Device;
static ID3D11DeviceContext* DeviceContext;
static ID3D11DeviceContext1* DeviceContext1;
static ID3D11ComputeShader* ComputeShader;
static ID3D11Buffer* ConstantBuffer;
static ID3D11UnorderedAccessView* RenderView;

static ID3D11Buffer* CellBuffer;
static ID3D11ShaderResourceView* CellView;

static ID3D11Texture2D* GlyphTexture;
static ID3D11ShaderResourceView* GlyphTextureView;

static ID3D11Buffer* GlyphMappingBuffer;
static ID3D11ShaderResourceView* GlyphMappingView;

typedef struct {
    int Char;
    uint32_t Foreground;
    uint32_t Background;
} TerminalCell;

typedef struct {
    uint32_t CellSize[2];
    uint32_t TermSize[2];
} RendererConstBuffer;

typedef struct {
    uint32_t GlyphIndex;
    uint32_t Foreground;
    uint32_t Background;
} RendererCell;

typedef struct {
    uint32_t Pos[2];
} GlyphMapping;

// max cell count in terminal (can be changed to dynamically reize)
#define REFTERM_MAX_WIDTH 1024
#define REFTERM_MAX_HEIGHT 1024

// max texture size to store rasterized glyphs (can also be resized, or changed to array texture)
#define REFTERM_TEXTURE_WIDTH 2048
#define REFTERM_TEXTURE_HEIGHT 2048

// mapping buffer size (max amoung of glyphs to support in one frame)
#define REFTERM_MAX_MAPPING 65536

static DWORD FontWidth;
static DWORD FontHeight;

static void RendererCreate(HWND Window)
{
    UINT Flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
#ifdef _DEBUG
    Flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL Levels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, Flags, Levels, ARRAYSIZE(Levels), D3D11_SDK_VERSION, &Device, NULL, &DeviceContext);
    if (FAILED(hr))
    {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, Flags, Levels, ARRAYSIZE(Levels), D3D11_SDK_VERSION, &Device, NULL, &DeviceContext);
    }
    AssertHR(hr);

    hr = ID3D11DeviceContext1_QueryInterface(DeviceContext, &IID_ID3D11DeviceContext1, &DeviceContext1);
    AssertHR(hr);

#ifdef _DEBUG
    ID3D11InfoQueue* Info;
    hr = IProvideClassInfo_QueryInterface(Device, &IID_ID3D11InfoQueue, (void**)&Info);
    AssertHR(hr);
    hr = ID3D11InfoQueue_SetBreakOnSeverity(Info, D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
    AssertHR(hr);
    hr = ID3D11InfoQueue_SetBreakOnSeverity(Info, D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
    AssertHR(hr);
    ID3D11InfoQueue_Release(Info);
#endif

    IDXGIDevice* DxgiDevice;
    hr = ID3D11Device_QueryInterface(Device, &IID_IDXGIDevice, (void**)&DxgiDevice);
    AssertHR(hr);

    IDXGIAdapter* DxgiAdapter;
    hr = IDXGIDevice_GetAdapter(DxgiDevice, &DxgiAdapter);
    AssertHR(hr);
    IDXGIDevice_Release(DxgiDevice);

    IDXGIFactory2* DxgiFactory;
    IDXGIAdapter_GetParent(DxgiAdapter, &IID_IDXGIFactory2, (void**)&DxgiFactory);
    AssertHR(hr);
    IDXGIAdapter_Release(DxgiAdapter);

    DXGI_SWAP_CHAIN_DESC1 SwapChainDesc =
    {
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS,
        .BufferCount = 2,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD, // use modern Win10 flip model
        .Scaling = DXGI_SCALING_NONE,
        .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
        .Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT,
    };

    IDXGISwapChain1* SwapChain1;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(DxgiFactory, (IUnknown*)Device, Window, &SwapChainDesc, NULL, NULL, &SwapChain1);
    AssertHR(hr);

    hr = IDXGISwapChain1_QueryInterface(SwapChain1, &IID_IDXGISwapChain2, &SwapChain);
    AssertHR(hr);
    IDXGISwapChain1_Release(SwapChain1);

    FrameLatencyWaitableObject = IDXGISwapChain2_GetFrameLatencyWaitableObject(SwapChain);

    IDXGIFactory2_MakeWindowAssociation(DxgiFactory, Window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    IDXGIFactory2_Release(DxgiFactory);

    hr = ID3D11Device_CreateComputeShader(Device, ReftermShaderBytes, sizeof(ReftermShaderBytes), NULL, &ComputeShader);
    AssertHR(hr);

    // cell & terminal size constants

    D3D11_BUFFER_DESC ConstantBufferDesc = {
        .ByteWidth = sizeof(RendererConstBuffer),
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    hr = ID3D11Device_CreateBuffer(Device, &ConstantBufferDesc, NULL, &ConstantBuffer);
    AssertHR(hr);

    // rendering cells

    D3D11_BUFFER_DESC CellBufferDesc = {
        .ByteWidth = REFTERM_MAX_WIDTH * REFTERM_MAX_HEIGHT * sizeof(RendererCell),
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
        .StructureByteStride = sizeof(RendererCell),
    };
    hr = ID3D11Device_CreateBuffer(Device, &CellBufferDesc, NULL, &CellBuffer);
    AssertHR(hr);

    D3D11_SHADER_RESOURCE_VIEW_DESC CellViewDesc = {
        .ViewDimension = D3D11_SRV_DIMENSION_BUFFER,
        .Buffer.FirstElement = 0,
        .Buffer.NumElements = REFTERM_MAX_WIDTH * REFTERM_MAX_HEIGHT,
    };
    hr = ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)CellBuffer, &CellViewDesc, &CellView);
    AssertHR(hr);

    // texture with glyphs

    D3D11_TEXTURE2D_DESC TextureDesc = {
        .Width = REFTERM_TEXTURE_WIDTH,
        .Height = REFTERM_TEXTURE_HEIGHT,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8X8_UNORM,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
    };
    hr = ID3D11Device_CreateTexture2D(Device, &TextureDesc, NULL, &GlyphTexture);
    AssertHR(hr);

    hr = ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)GlyphTexture, NULL, &GlyphTextureView);
    AssertHR(hr);

    // glyph mapping buffer

    D3D11_BUFFER_DESC MappingDesc = {
        .ByteWidth = REFTERM_MAX_MAPPING * sizeof(GlyphMapping),
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
        .StructureByteStride = sizeof(GlyphMapping),
    };
    hr = ID3D11Device_CreateBuffer(Device, &MappingDesc, NULL, &GlyphMappingBuffer);
    AssertHR(hr);
    
    D3D11_SHADER_RESOURCE_VIEW_DESC MappingViewDesc = {
        .ViewDimension = D3D11_SRV_DIMENSION_BUFFER,
        .Buffer.FirstElement = 0,
        .Buffer.NumElements = REFTERM_MAX_MAPPING,
    };
    hr = ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)GlyphMappingBuffer, &MappingViewDesc, &GlyphMappingView);
    AssertHR(hr);

    // TEMPORARY - fill in texture & mapping with 128 ASCII symbols rendered with GDI
    {
        FontHeight = 20;
        LPWSTR FontName = L"Consolas";
        BOOL ClearType = TRUE; // asking for cleartype will make GDI rasterize separate weights for red, green and blue

        BITMAPINFOHEADER BitmapHeader = {
            .biSize = sizeof(BitmapHeader),
            .biWidth = REFTERM_TEXTURE_WIDTH,
            .biHeight = -REFTERM_TEXTURE_HEIGHT,
            .biPlanes = 1,
            .biBitCount = 32,
            .biCompression = BI_RGB,
        };

        HDC DC = CreateCompatibleDC(0);
        Assert(DC);

        void* Pixels;
        HBITMAP Bitmap = CreateDIBSection(DC, &(BITMAPINFO){ BitmapHeader }, DIB_RGB_COLORS, &Pixels, NULL, 0);
        Assert(Bitmap);
        SelectObject(DC, Bitmap);

        HFONT Font = CreateFontW(
            FontHeight, 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            ClearType ? CLEARTYPE_QUALITY : ANTIALIASED_QUALITY, FIXED_PITCH, FontName);
        Assert(Font);
        SelectObject(DC, Font);

        TEXTMETRICW Metrics;
        BOOL ok = GetTextMetricsW(DC, &Metrics);
        Assert(ok);

        FontWidth = Metrics.tmAveCharWidth + 1; // not sure why +1 is needed here

        SetTextColor(DC, RGB(255, 255, 255));
        SetBkColor(DC, RGB(0, 0, 0));

        GlyphMapping Mapping[128];

        for (int Index=0; Index<128; Index++)
        {
            RECT Rect = { Index*FontWidth, 0, Index*FontWidth + FontWidth, FontHeight };
            WCHAR Char = (WCHAR)Index;
            ExtTextOutW(DC, Rect.left, Rect.top, ETO_OPAQUE, &Rect, &Char, 1, NULL);

            Mapping[Index] = (GlyphMapping){ Rect.left, Rect.top };
        }

        // upload subset of texture to GPU
        {
            D3D11_BOX Box = {
                .left = 0,
                .right = 128 * FontWidth,
                .top = 0,
                .bottom = FontHeight,
                .front = 0,
                .back = 1,
            };

            UINT Pitch = REFTERM_TEXTURE_WIDTH * 4; // RGBA bitmap
            ID3D11DeviceContext_UpdateSubresource(DeviceContext, (ID3D11Resource*)GlyphTexture, 0, &Box, Pixels, Pitch, 0);
        }

        // upload subset of mapping to GPU
        {
            D3D11_BOX Box = {
                .left = 0,
                .right = 128 * sizeof(GlyphMapping),
                .top = 0,
                .bottom = 1,
                .front = 0,
                .back = 1,
            };

            ID3D11DeviceContext_UpdateSubresource(DeviceContext, (ID3D11Resource*)GlyphMappingBuffer, 0, &Box, Mapping, 0, 0);
        }
    }
}

static DWORD CurrentWidth;
static DWORD CurrentHeight;

static void RendererDraw(DWORD Width, DWORD Height, TerminalCell* Terminal, DWORD TermWidth, DWORD TermHeight)
{
    HRESULT hr;

    // resize RenderView to match window size
    if (Width != CurrentWidth || Height != CurrentHeight)
    {
        ID3D11DeviceContext_ClearState(DeviceContext);

        if (RenderView)
        {
            ID3D11UnorderedAccessView_Release(RenderView);
            ID3D11DeviceContext_Flush(DeviceContext);
            RenderView = NULL;
        }

        if (Width != 0 && Height != 0)
        {
            hr = IDXGISwapChain_ResizeBuffers(SwapChain, 0, Width, Height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
            AssertHR(hr);

            ID3D11Texture2D* Buffer;
            hr = IDXGISwapChain_GetBuffer(SwapChain, 0, &IID_ID3D11Texture2D, (void**)&Buffer);
            AssertHR(hr);
            hr = ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)Buffer, NULL, &RenderView);
            AssertHR(hr);
            ID3D11Texture2D_Release(Buffer);
        }

        CurrentWidth = Width;
        CurrentHeight = Height;
    }

    if (RenderView)
    {
        D3D11_MAPPED_SUBRESOURCE Mapped;
        hr = ID3D11DeviceContext_Map(DeviceContext, (ID3D11Resource*)ConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped);
        AssertHR(hr);
        {
            RendererConstBuffer ConstData =
            {
                .CellSize = { FontWidth, FontHeight },
                .TermSize = { TermWidth, TermHeight },
            };
            memcpy(Mapped.pData, &ConstData, sizeof(ConstData));
        }
        ID3D11DeviceContext_Unmap(DeviceContext, (ID3D11Resource*)ConstantBuffer, 0);

        hr = ID3D11DeviceContext_Map(DeviceContext, (ID3D11Resource*)CellBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped);
        AssertHR(hr);
        {
            RendererCell* Cells = Mapped.pData;

            DWORD CellCount = TermWidth * TermHeight;
            for (DWORD Index = 0; Index < CellCount; Index++)
            {
                TerminalCell Cell = Terminal[Index];

                uint32_t GlyphIndex = Cell.Char; // TODO: do the mapping here

                *Cells++ = (RendererCell){
                    .GlyphIndex = GlyphIndex,
                    .Foreground = Cell.Foreground,
                    .Background = Cell.Background,
                };
            }
        }
        ID3D11DeviceContext_Unmap(DeviceContext, (ID3D11Resource*)CellBuffer, 0);

        // this should match t0/t1/t2 order in hlsl shader
        ID3D11ShaderResourceView* Resources[] = { CellView, GlyphMappingView, GlyphTextureView };

        ID3D11DeviceContext_CSSetConstantBuffers(DeviceContext, 0, 1, &ConstantBuffer);
        ID3D11DeviceContext_CSSetShaderResources(DeviceContext, 0, ARRAYSIZE(Resources), Resources);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(DeviceContext, 0, 1, &RenderView, NULL);
        ID3D11DeviceContext_CSSetShader(DeviceContext, ComputeShader, NULL, 0);

        // this issues compute shader for window size, which in real terminal should match its size
        ID3D11DeviceContext_Dispatch(DeviceContext, (Width + 7) / 8, (Height + 7) / 8, 1);
    }

    BOOL Vsync = FALSE;
    hr = IDXGISwapChain1_Present(SwapChain, Vsync ? 1 : 0, 0);
    if (hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_REMOVED)
    {
        MessageBoxW(NULL, L"D3D11 Device Lost!", L"Error", MB_ICONERROR);
        ExitProcess(0);
    }
    else
    {
        AssertHR(hr);
    }

    ID3D11DeviceContext1_DiscardView(DeviceContext1, (ID3D11View*)RenderView);
}

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    switch (Message)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(Window, Message, WParam, LParam);
}

void WinMainCRTStartup()
{
    WNDCLASSEXW WindowClass = {
        .cbSize = sizeof(WindowClass),
        .lpfnWndProc = &WindowProc,
        .hInstance = GetModuleHandleW(NULL),
        .hIcon = LoadIconA(NULL, IDI_APPLICATION),
        .hCursor = LoadCursorA(NULL, IDC_ARROW),
        .hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH),
        .lpszClassName = L"reftermclass",
    };
    ATOM Atom = RegisterClassExW(&WindowClass);
    Assert(Atom);

    DWORD ExStyle = WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP; // magic style to make DXGI_SWAP_EFFECT_FLIP_DISCARD not glitch on window resizing
    DWORD Style = WS_OVERLAPPEDWINDOW;
    HWND Window = CreateWindowExW(
        ExStyle, WindowClass.lpszClassName, L"refterm", Style,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL, WindowClass.hInstance, NULL);
    Assert(Window);

    RendererCreate(Window);
    ShowWindow(Window, SW_SHOWDEFAULT);

    // TEMPORARY - dummy terminal, hardcoded sizes, should get it from window size probably
    #define TermWidth 312
    #define TermHeight 88
    static TerminalCell Terminal[TermWidth*TermHeight];

    LARGE_INTEGER Frequency, Time;
    QueryPerformanceFrequency(&Frequency);
    QueryPerformanceCounter(&Time);

    uint32_t Frames = 0;
    int64_t UpdateTitle = Time.QuadPart + Frequency.QuadPart;

    for (;;)
    {
        // TEMPORARY - update terminal contents
        {
            static int Offset = 0;
            // first row
            for (int Index = 32; Index < 128; Index++)
            {
                Terminal[0 * TermWidth + (Index + Offset) % TermWidth] =
                    (TerminalCell){ (WCHAR)Index, 0xcccccc, 0x000000 }; // gray on black
            }

            // random color stuff
            for (int Y = 1; Y < TermHeight; Y++)
            {
                for (int X = 0; X < TermWidth; X++)
                {
                    WCHAR Char = (WCHAR)(32 + (X + Y) % (128 - 32));
                    uint32_t Foreground = 0x010305 * (X + Y * 33);
                    uint32_t Background = 0x27394b * Y;
                    Terminal[Y * TermWidth + (X + Offset) % TermWidth] =
                        (TerminalCell){ Char, Foreground, Background };
                }
            }
        }
        // uncomment to scroll very fast
        //Offset = (Offset + 1) % TermWidth;

        for (;;)
        {
            HANDLE Events[] = { FrameLatencyWaitableObject };
            DWORD Wait = MsgWaitForMultipleObjects(ARRAYSIZE(Events), Events, FALSE, INFINITE, QS_ALLEVENTS);
            if (Wait == WAIT_OBJECT_0)
            {
                // render only when GPU can flip
                break;
            }
            else if (Wait == WAIT_OBJECT_0 + 1)
            {
                MSG Message;
                while (PeekMessageW(&Message, NULL, 0, 0, PM_REMOVE))
                {
                    if (Message.message == WM_QUIT)
                    {
                        ExitProcess(0);
                    }
                    TranslateMessage(&Message);
                    DispatchMessageW(&Message);
                }
            }
        }

        RECT Rect;
        GetClientRect(Window, &Rect);

        DWORD Width = Rect.right - Rect.left;
        DWORD Height = Rect.bottom - Rect.top;
        RendererDraw(Width, Height, Terminal, TermWidth, TermHeight);
        Frames++;

        LARGE_INTEGER Now;
        QueryPerformanceCounter(&Now);

        if (Now.QuadPart >= UpdateTitle)
        {
            UpdateTitle = Now.QuadPart + Frequency.QuadPart;

            double FramesPerSec = (double)Frames * Frequency.QuadPart / (Now.QuadPart - Time.QuadPart);
            Time = Now;
            Frames = 0;

            WCHAR Title[1024];
            wsprintfW(Title, L"refterm Size=%dx%d RenderFPS=%d.%02d", TermWidth, TermHeight, (int)FramesPerSec, (int)(FramesPerSec*100) % 100);
            SetWindowTextW(Window, Title);
        }
    }
}

// CRT stuff

#pragma function(memset)
void* memset(void* Dst, int Src, size_t Size)
{
    __stosb((unsigned char*)Dst, (unsigned char)Src, Size);
    return Dst;
}

#pragma function(memcpy)
void* memcpy(void* Dst, const void* Src, size_t Size)
{
    __movsb((unsigned char*)Dst, (unsigned char*)Src, Size);
    return Dst;
}
