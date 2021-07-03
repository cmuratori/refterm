static void GDIInit(glyph_generator *GlyphGen)
{
    BITMAPINFOHEADER BitmapHeader =
    {
        .biSize = sizeof(BitmapHeader),
        .biWidth = GlyphGen->TransferWidth,
        .biHeight = -(int)GlyphGen->TransferHeight,
        .biPlanes = 1,
        .biBitCount = 32,
        .biCompression = BI_RGB,
    };
    
    GlyphGen->DC = CreateCompatibleDC(0);
    Assert(GlyphGen->DC);
    
    void* Pixels;
    GlyphGen->Bitmap = CreateDIBSection(GlyphGen->DC, &(BITMAPINFO){BitmapHeader}, DIB_RGB_COLORS, &Pixels, 0, 0);
    Assert(GlyphGen->Bitmap);
    SelectObject(GlyphGen->DC, GlyphGen->Bitmap);
    
    GlyphGen->Pixels = (uint32_t *)Pixels;
    GlyphGen->Pitch = 4*GlyphGen->TransferWidth;
    
    SetTextColor(GlyphGen->DC, RGB(255, 255, 255));
    SetBkColor(GlyphGen->DC, RGB(0, 0, 0));
}

static int GDISetFont(glyph_generator *GlyphGen, wchar_t *FontName, uint32_t FontHeight)
{
    int Result = 0;
    
    if(GlyphGen->Font)
    {
        SelectObject(GlyphGen->DC, GlyphGen->OldFont);
        DeleteObject(GlyphGen->Font);
    }
    
    GlyphGen->Font = CreateFontW(FontHeight, 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                                 GlyphGen->UseClearType ? CLEARTYPE_QUALITY : ANTIALIASED_QUALITY, FIXED_PITCH, FontName);
    if(GlyphGen->Font)
    {
        GlyphGen->OldFont = SelectObject(GlyphGen->DC, GlyphGen->Font);
        
        TEXTMETRICW Metrics;
        GetTextMetricsW(GlyphGen->DC, &Metrics);
        
        // TODO(casey): Real cell size determination would go here - probably with input from the user?
        GlyphGen->FontHeight = Metrics.tmHeight;
        GlyphGen->FontWidth = Metrics.tmAveCharWidth + 1; // not sure why +1 is needed here
        // GlyphGen.FontWidth = Metrics.tmMaxCharWidth; // not sure why +1 is needed here
        
        Result = 1;
    }
    
    return Result;
}

static int SetFont(glyph_generator *GlyphGen, uint32_t Flags, wchar_t *FontName, uint32_t FontHeight)
{
    int Result = 0;
    
    GlyphGen->UseClearType = Flags & GlyphGen_UseClearType;
    GlyphGen->UseDWrite = 0;
    
    if(Flags & GlyphGen_UseDirectWrite)
    {
        if(DWriteSetFont(GlyphGen, FontName, FontHeight))
        {
            Result = 1;
            GlyphGen->UseDWrite = 1;
        }
    }
    
    if(!Result)
    {
        Result = GDISetFont(GlyphGen, FontName, FontHeight);
    }
    
    return Result;
}

static glyph_generator AllocateGlyphGenerator(uint32_t TransferWidth, uint32_t TransferHeight,
                                              IDXGISurface *GlyphTransferSurface)
{
    glyph_generator GlyphGen = {0};

    GlyphGen.TransferWidth = TransferWidth;
    GlyphGen.TransferHeight = TransferHeight;
    
    GDIInit(&GlyphGen);
    DWriteInit(&GlyphGen, GlyphTransferSurface);
    
    return GlyphGen;
}

static uint32_t GetExpectedTileCountForDimension(glyph_generator *GlyphGen, uint32_t Width, uint32_t Height)
{
    uint32_t PerRow = SafeRatio1(Width, GlyphGen->FontWidth);
    uint32_t PerColumn = SafeRatio1(Height, GlyphGen->FontHeight);
    uint32_t Result = PerRow*PerColumn;
    
    return Result;
}

static uint32_t GetTileCount(glyph_generator *GlyphGen, glyph_table *Table, size_t Count, wchar_t *String, glyph_hash RunHash)
{
    /* TODO(casey): Windows can only 2^31 glyph runs - which
       seems fine, but... technically Unicode can have more than two
       billion combining characters, so I guess theoretically this
       code is broken - another "reason" to do a custom glyph rasterizer? */
    DWORD StringLen = (DWORD)Count;
    Assert(StringLen == Count);
    
    glyph_state Entry = FindGlyphEntryByHash(Table, RunHash);
    if(Entry.FilledState == GlyphState_None)
    {
        if(StringLen)
        {
            SIZE Size = {0};
            if(GlyphGen->UseDWrite)
            {
                Size = DWriteGetTextExtent(GlyphGen, StringLen, String);
            }
            else
            {
                GetTextExtentPointW(GlyphGen->DC, String, StringLen, &Size);
            }
            
            Entry.TileCount = SafeRatio1((uint16_t)(Size.cx + GlyphGen->FontWidth/2), GlyphGen->FontWidth);
        }
        else
        {
            Entry.TileCount = 0;
        }
        
        UpdateGlyphCacheEntry(Table, Entry.ID, GlyphState_Sized, Entry.TileCount);
    }
    
    uint16_t Result = Entry.TileCount;
    return Result;
}

static void PrepareTilesForTransfer(glyph_generator *GlyphGen, d3d11_renderer *Renderer, size_t Count, wchar_t *String, uint32_t TileCount)
{
    DWORD StringLen = (DWORD)Count;
    Assert(StringLen == Count);

    if(TileCount)
    {
        if(GlyphGen->UseDWrite)
        {
            DWriteDrawText(GlyphGen, StringLen, String, 0, 0, GlyphGen->TransferWidth, GlyphGen->TransferHeight,
                           Renderer->DWriteRenderTarget, Renderer->DWriteFillBrush);
        }
        else
        {
            PatBlt(GlyphGen->DC, 0, 0, TileCount*GlyphGen->FontWidth, GlyphGen->FontHeight, BLACKNESS);
            if(!ExtTextOutW(GlyphGen->DC, 0, 0, ETO_OPAQUE, 0, String, StringLen, 0))
            {
                DWORD Error = GetLastError();
                DWORD TestError = Error;
                Assert(!"ExtTextOutW failure");
            }
        }
    }
}
 
static void TransferTile(glyph_generator *GlyphGen, d3d11_renderer *Renderer, uint32_t TileIndex, gpu_glyph_index DestIndex)
{
    /* TODO(casey):
    
       Regardless of whether DirectWrite or GDI is used, rasterizing glyphs via Windows' libraries is extremely slow.
       
       It may appear that this code path itself is the reason for the slowness, because this does a very inefficient
       "draw-then-transfer" for every glyph group, which is the slowest possible way you could do it.  However, I
       actually _tried_ doing batching, where you make a single call to DirectWrite or GDI to rasterize 
       large sets of glyphs which are then transfered all at once.
       
       Although this does help the performance (IIRC there was about 2x total speedup in heavy use),
       the peformance is still about two orders of magnitude away from where it should be.  So I removed the batching, 
       because it complicates the code quite a bit, and does not actually produce acceptable performance.
       
       I believe the only solution to actual fast glyph generation is to just write something that isn't as
       bad as GDI/DirectWrite.  It's a waste of code complexity to try to get a reasonable speed out of them, unless
       someone else manages to find some magic switches I didn't find that make them work at a high speed in
       bulk.
    */
    
    if(Renderer->DeviceContext)
    {
        glyph_cache_point Point = UnpackGlyphCachePoint(DestIndex);
        uint32_t X = Point.X*GlyphGen->FontWidth;
        uint32_t Y = Point.Y*GlyphGen->FontHeight;
        
        if(GlyphGen->UseDWrite)
        {
            D3D11_BOX SourceBox =
            {
                .left = (TileIndex)*GlyphGen->FontWidth,
                .right = (TileIndex + 1)*GlyphGen->FontWidth,
                .top = 0,
                .bottom = GlyphGen->FontHeight,
                .front = 0,
                .back = 1,
            };
            
            ID3D11DeviceContext_CopySubresourceRegion(Renderer->DeviceContext,
                                                      (ID3D11Resource *)Renderer->GlyphTexture, 0, X, Y, 0,
                                                      (ID3D11Resource *)Renderer->GlyphTransfer, 0, &SourceBox);
        }
        else
        {
            D3D11_BOX TexelBox = 
            {
                .left = X,
                .right = X + GlyphGen->FontWidth,
                .top = Y,
                .bottom = Y + GlyphGen->FontHeight,
                .front = 0,
                .back = 1,
            };
            
            Assert(((TileIndex + 1)*GlyphGen->FontWidth) <= GlyphGen->TransferWidth);
            
            uint32_t *SourceCorner = GlyphGen->Pixels + TileIndex*GlyphGen->FontWidth;
            ID3D11DeviceContext_UpdateSubresource(Renderer->DeviceContext, (ID3D11Resource*)Renderer->GlyphTexture,
                                                  0, &TexelBox, SourceCorner, GlyphGen->Pitch, 0);
        }
    }
}
    