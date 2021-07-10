#include <dwrite.h>
#include <d2d1.h>
#include <stddef.h>
#include <stdint.h>

extern "C"
{
#include "refterm.h"
#include "refterm_example_glyph_generator.h"
}

extern "C" int D2DAcquire(IDXGISurface *GlyphTransferSurface,
                          struct ID2D1RenderTarget **DWriteRenderTarget,
                          struct ID2D1SolidColorBrush **DWriteFillBrush)
{
    int Result = 0;
    
    // TODO(casey): Obey "ClearType" here.
    
    // TODO(casey): Not sure about these props...
    D2D1_RENDER_TARGET_PROPERTIES Props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                                       0, 0);
    
    ID2D1Factory *Factory = 0;
    D2D1_FACTORY_OPTIONS Options = {};
    Options.debugLevel = D2D1_DEBUG_LEVEL_ERROR;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &Options, (void**)&Factory);
    if(Factory)
    {
        Factory->CreateDxgiSurfaceRenderTarget(GlyphTransferSurface, &Props, DWriteRenderTarget);
        if(*DWriteRenderTarget)
        {
            (*DWriteRenderTarget)->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWriteFillBrush);
            Result = (*DWriteFillBrush != 0);
        }
        
        Factory->Release();
    }
    
    return Result;
}

extern "C" void D2DRelease(struct ID2D1RenderTarget **DWriteRenderTarget,
                           struct ID2D1SolidColorBrush **DWriteFillBrush)
{
    if(*DWriteFillBrush)
    {
        (*DWriteFillBrush)->Release();
        *DWriteFillBrush = 0;
    }
    
    if(*DWriteRenderTarget)
    {
        (*DWriteRenderTarget)->Release();
        *DWriteRenderTarget = 0;
    }
}

extern "C" int DWriteInit(glyph_generator *GlyphGen, IDXGISurface *GlyphTransferSurface)
{
    int Result = 0;
    
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&GlyphGen->DWriteFactory);
    if(GlyphGen->DWriteFactory)
    {
        Result = 1;
    }
    
    return Result;
}

extern "C" SIZE DWriteGetTextExtent(glyph_generator *GlyphGen, int StringLen, wchar_t *String)
{
    SIZE Result = {0};
    
    IDWriteTextLayout *Layout = 0;
    GlyphGen->DWriteFactory->CreateTextLayout(String, StringLen, GlyphGen->TextFormat,
                                              (float)GlyphGen->TransferWidth, (float)GlyphGen->TransferHeight, &Layout);
    if(Layout)
    {
        DWRITE_TEXT_METRICS Metrics = {0};
        Layout->GetMetrics(&Metrics);
        Result.cx = (uint32_t)Metrics.width;
        Result.cy = (uint32_t)Metrics.height;
        
        Layout->Release();
    }
    
    return Result;
}

void DWriteReleaseFont(glyph_generator *GlyphGen)
{
    if(GlyphGen->FontFace)
    {
        GlyphGen->FontFace->Release();
        GlyphGen->FontFace = 0;
    }
    }

extern "C" int DWriteSetFont(glyph_generator *GlyphGen, wchar_t *FontName, uint32_t FontHeight)
{
    int Result = 0;
    
    DWriteReleaseFont(GlyphGen);
    
    if(GlyphGen->DWriteFactory)
    {
        GlyphGen->DWriteFactory->CreateTextFormat(FontName,
                                                  0,
                                                  DWRITE_FONT_WEIGHT_REGULAR,
                                                  DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL,
                                                  (float)FontHeight,
                                                  L"en-us", // TODO(casey): Where do I get this from?  Normally locales are IDs, but this is a string for some reason???
                                                  &GlyphGen->TextFormat);
        if(GlyphGen->TextFormat)
        {
            GlyphGen->TextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            GlyphGen->TextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            
            wchar_t M = L'M';
            IDWriteTextLayout *Layout = 0;
            GlyphGen->DWriteFactory->CreateTextLayout(&M, 1, GlyphGen->TextFormat,
                                                      (float)GlyphGen->TransferWidth, (float)GlyphGen->TransferHeight, &Layout);
            if(Layout)
            {
                // TODO(casey): Real cell size determination would go here - probably with input from the user?
                DWRITE_TEXT_METRICS CharMetrics = {0};
                Layout->GetMetrics(&CharMetrics);
                
                DWRITE_LINE_METRICS LineMetrics = {0};
                UINT32 Ignored;
                Layout->GetLineMetrics(&LineMetrics, 1, &Ignored);
                
                GlyphGen->GlyphHeight = (uint32_t)(1.5*LineMetrics.height);
                GlyphGen->GlyphWidth = (uint32_t)(1.5*CharMetrics.width);
                GlyphGen->CellHeight = (uint32_t)LineMetrics.height;
                GlyphGen->CellWidth = (uint32_t)CharMetrics.width;
                
                Layout->Release();
                
                Result = 1;
            }
        }
    }
    
    return Result;
}

extern "C" void DWriteDrawText(glyph_generator *GlyphGen, int StringLen, WCHAR *String,
                               uint32_t Left, uint32_t Top, uint32_t Right, uint32_t Bottom,
                               struct ID2D1RenderTarget *RenderTarget,
                               struct ID2D1SolidColorBrush *FillBrush)
{
    D2D1_RECT_F Rect;
    
    Rect.left = (float)Left;
    Rect.top = (float)Top;
    Rect.right = (float)Right;
    Rect.bottom = (float)Bottom;

    RenderTarget->BeginDraw();
    RenderTarget->Clear();
    RenderTarget->DrawText(String, StringLen, GlyphGen->TextFormat, &Rect, FillBrush,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP|D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT, DWRITE_MEASURING_MODE_NATURAL);
    HRESULT Error = RenderTarget->EndDraw();
    if(!SUCCEEDED(Error))
    {
        Assert(!"EndDraw failed");
    }
}

extern "C" void DWriteRelease(glyph_generator *GlyphGen)
{
    /* NOTE(casey): There is literally no point to this function
       whatsoever except to stop the D3D debug runtime from 
       complaining about unreleased resources when the program
       exits.  EVEN THOUGH THEY WOULD BE AUTOMATICALLY RELEASED
       AT THAT TIME.  So now here I am manually releasing them,
       which wastes the user's time, for no reason at all. */
    
    DWriteReleaseFont(GlyphGen);
    if(GlyphGen->DWriteFactory) GlyphGen->DWriteFactory->Release();
}
