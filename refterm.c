/* TODO(casey): Reminders

   1) Probably just double-map the source buffer, that's
      probably what people should do.
   2) Split RendererDraw into two things - update and render,
      since we don't actually have to update anything if
      the terminal cells don't change.
   3) Use VirtualAlloc2 for source buffer
   4) Add actual choice-of-two eviction (w/ AES random)
*/

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

// max cell count in terminal (can be changed to dynamically reize)
#define REFTERM_MAX_WIDTH 1024
#define REFTERM_MAX_HEIGHT 1024

// max texture size to store rasterized glyphs (can also be resized, or changed to array texture)
#define REFTERM_TEXTURE_WIDTH 2048
#define REFTERM_TEXTURE_HEIGHT 2048

// mapping buffer size (max amoung of glyphs to support in one frame)
#define REFTERM_MAX_MAPPING 65536

#define Assert(cond) do { if (!(cond)) __debugbreak(); } while (0)
#define AssertHR(hr) Assert(SUCCEEDED(hr))
#define ArrayCount(Array) (sizeof(Array) / sizeof((Array)[0]))

#define IsPowerOfTwo(Value) (((Value) & ((Value) - 1)) == 0)

#include "refterm_shader.h"
#include "refterm_glyph_cache.h"
#include "refterm_source_buffer.h"
#include "refterm_glyph_generator.h"
#include "refterm.h"
#include "refterm_glyph_cache.c"
#include "refterm_source_buffer.c"
#include "refterm_glyph_generator.c"

#pragma comment (lib, "kernel32.lib")
#pragma comment (lib, "user32.lib")
#pragma comment (lib, "gdi32.lib")
#pragma comment (lib, "dxguid.lib")
#pragma comment (lib, "d3d11.lib")

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

static TerminalBuffer AllocateTerminalBuffer(int DimX, int DimY)
{
    TerminalBuffer Result = {0};

    size_t TotalSize = sizeof(TerminalCell)*DimX*DimY;
    Result.Cells = VirtualAlloc(0, TotalSize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if(Result.Cells)
    {
        Result.DimX = DimX;
        Result.DimY = DimY;
    }

    return Result;
}

static void DeallocateTerminalBuffer(TerminalBuffer *Buffer)
{
    if(Buffer && Buffer->Cells)
    {
        VirtualFree(Buffer->Cells, 0, MEM_RELEASE);
        Buffer->DimX = Buffer->DimY = 0;
        Buffer->Cells = 0;
    }
}


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
}

static DWORD CurrentWidth;
static DWORD CurrentHeight;

static void RendererDraw(glyph_generator *GlyphGen, SourceBuffer *Source, GlyphTable *Table, DWORD Width, DWORD Height, TerminalBuffer *Term, size_t FrameIndex)
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
                .CellSize = { GlyphGen->FontWidth, GlyphGen->FontHeight },
                .TermSize = { Term->DimX, Term->DimY },
            };
            memcpy(Mapped.pData, &ConstData, sizeof(ConstData));
        }
        ID3D11DeviceContext_Unmap(DeviceContext, (ID3D11Resource*)ConstantBuffer, 0);

        hr = ID3D11DeviceContext_Map(DeviceContext, (ID3D11Resource*)CellBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped);
        AssertHR(hr);
        {
            RendererCell* Cells = Mapped.pData;

            DWORD CellCount = Term->DimX * Term->DimY;
            for (DWORD Index = 0; Index < CellCount; Index++)
            {
                TerminalCell Cell = Term->Cells[Index];

                GlyphEntry *Entry = GetOrFillCache(GlyphGen, DeviceContext, Source, Table, Cell.AbsoluteP, Cell.RunCount, Cell.RunHash, FrameIndex);
                Assert(Cell.TileIndex < Entry->IndexCount);
                Assert(Cell.TileIndex < ArrayCount(Entry->GPUIndexes));

                *Cells++ = (RendererCell){
                    .GlyphIndex = Entry->GPUIndexes[Cell.TileIndex],
                    .Foreground = Cell.Foreground,
                    .Background = Cell.Background,
                };
            }
        }
        ID3D11DeviceContext_Unmap(DeviceContext, (ID3D11Resource*)CellBuffer, 0);

        // this should match t0/t1/t2 order in hlsl shader
        ID3D11ShaderResourceView* Resources[] = { CellView, GlyphTextureView };

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

static void UpdateExampleGraphemeParser(ExampleParser *Parser, SourceBuffer *Buffer)
{
}

static void UpdateTerminalBlahBlah(SourceBuffer *Source, TerminalBuffer *Term)
{
    #if 0
    // TODO(casey): We would like this to maybe be based on
    // time?
    int ReadsPerRefresh = 4;
    for(int ReadIndex = 0;
        ReadIndex < ReadsPerRefresh;
        ++ReadIndex)
    {
        SourceBufferRange WriteRange = GetLargestWriteRange(&ScrollBackBuffer);
        if(ReadFromPipe(Pipe, WriteRange.SizeA, WriteRange.DestA))
        {
            UpdateExampleGraphemeParser(&Parser, &ScrollBackBuffer, &TerminalBuffer);
        }
        else
        {
            break;
        }
    }
    #endif

    int CellIndex = 0;
    for (uint32_t Y = 0; Y < Term->DimY; Y++)
    {
        for (uint32_t X = 0; X < Term->DimX; X++)
        {
            TerminalCell *Cell = Term->Cells + CellIndex;
            Cell->Foreground = 0x010305 * (X + Y * 33);
            Cell->Background = 0x27394b * Y;

            ((WCHAR *)Source->Data)[CellIndex] = CellIndex;
            Cell->AbsoluteP = 2*CellIndex;
            Cell->RunCount = 2;
            Cell->TileIndex = 0;

            Cell->RunHash = ComputeGlyphHash(Source, Cell->AbsoluteP, Cell->RunCount, DefaultSeed);
        }
    }

    Source->AbsoluteFilledSize = CellIndex;
}

void WinMainCRTStartup()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);

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
    glyph_generator GlyphGen = AllocateGlyphGenerator(L"Consolas", 20, GlyphTexture);
    ShowWindow(Window, SW_SHOWDEFAULT);

    LARGE_INTEGER Frequency, Time;
    QueryPerformanceFrequency(&Frequency);
    QueryPerformanceCounter(&Time);

    size_t FrameCount = 0;
    size_t FrameIndex = 1;
    int64_t UpdateTitle = Time.QuadPart + Frequency.QuadPart;

    ExampleParser Parser = {0};
    Parser.LastAbsoluteP = 1;
    SourceBuffer ScrollBackBuffer = AllocateSourceBuffer(1024*1024);

    GlyphTableParams Params = {0};
    Params.HashCount = 4096;
    Params.AssocCount = 16;
    Params.IndexCount = 65536;

    GlyphTable Table = AllocateGlyphTable(Params, REFTERM_TEXTURE_WIDTH / GlyphGen.FontWidth);
    TerminalBuffer TermBuffer = {0};

    for (;;)
    {
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

        if((Rect.right >= Rect.left) &&
           (Rect.bottom >= Rect.top))
        {
            DWORD Width = Rect.right - Rect.left;
            DWORD Height = Rect.bottom - Rect.top;

            uint32_t NewDimX = (Width + GlyphGen.FontWidth - 1) / GlyphGen.FontWidth;
            uint32_t NewDimY = (Height + GlyphGen.FontHeight - 1) / GlyphGen.FontHeight;
            if(NewDimX > REFTERM_MAX_WIDTH) NewDimX = REFTERM_MAX_WIDTH;
            if(NewDimY > REFTERM_MAX_HEIGHT) NewDimY = REFTERM_MAX_HEIGHT;

            // TODO(casey): Maybe only allocate on size differences,
            // etc. Make a real resize function here for people who care.
            if((TermBuffer.DimX != NewDimX) &&
               (TermBuffer.DimY != NewDimY))
            {
                DeallocateTerminalBuffer(&TermBuffer);
                TermBuffer = AllocateTerminalBuffer(NewDimX, NewDimY);
                UpdateTerminalBlahBlah(&ScrollBackBuffer, &TermBuffer);
            }

            // TODO(casey): Split RendererDraw into two!
            // Update, and render, since we only need to update
            // if we actually get new input.
            RendererDraw(&GlyphGen, &ScrollBackBuffer, &Table, Width, Height, &TermBuffer, FrameIndex);
            ++FrameIndex;
            ++FrameCount;
        }

        LARGE_INTEGER Now;
        QueryPerformanceCounter(&Now);

        if (Now.QuadPart >= UpdateTitle)
        {
            UpdateTitle = Now.QuadPart + Frequency.QuadPart;

            double FramesPerSec = (double)FrameCount * Frequency.QuadPart / (Now.QuadPart - Time.QuadPart);
            Time = Now;
            FrameCount = 0;

            WCHAR Title[1024];
            wsprintfW(Title, L"refterm Size=%dx%d RenderFPS=%d.%02d", TermBuffer.DimX, TermBuffer.DimY, (int)FramesPerSec, (int)(FramesPerSec*100) % 100);
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
