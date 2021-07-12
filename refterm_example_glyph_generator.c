static int SetFont(glyph_generator *GlyphGen, wchar_t *FontName, uint32_t FontHeight)
{
    int Result = DWriteSetFont(GlyphGen, FontName, FontHeight);
    return Result;
}

static glyph_generator AllocateGlyphGenerator(uint32_t TransferWidth, uint32_t TransferHeight,
                                              IDXGISurface *GlyphTransferSurface)
{
    glyph_generator GlyphGen = {0};

    GlyphGen.TransferWidth = TransferWidth;
    GlyphGen.TransferHeight = TransferHeight;

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

static glyph_dim GetSingleTileUnitDim(void)
{
    glyph_dim Result = {1, 1.0f, 1.0f};
    return Result;
}

static glyph_dim GetGlyphDim(glyph_generator *GlyphGen, glyph_table *Table, size_t Count, wchar_t *String, glyph_hash RunHash)
{
    /* TODO(casey): Windows can only 2^31 glyph runs - which
       seems fine, but... technically Unicode can have more than two
       billion combining characters, so I guess theoretically this
       code is broken - another "reason" to do a custom glyph rasterizer? */

    glyph_dim Result = {0};

    DWORD StringLen = (DWORD)Count;
    Assert(StringLen == Count);

    SIZE Size = {0};
    glyph_state Entry = FindGlyphEntryByHash(Table, RunHash);
    if(Entry.FilledState == GlyphState_None)
    {
        if(StringLen)
        {
            Size = DWriteGetTextExtent(GlyphGen, StringLen, String);
        }

        UpdateGlyphCacheEntry(Table, Entry.ID, GlyphState_Sized, (uint16_t)Size.cx, (uint16_t)Size.cy);
    }
    else
    {
        Size.cx = Entry.DimX;
        Size.cy = Entry.DimY;
    }

    Result.TileCount = SafeRatio1((uint16_t)(Size.cx + GlyphGen->FontWidth/2), GlyphGen->FontWidth);
    
    Result.XScale = 1.0f;
    if((uint32_t)Size.cx > GlyphGen->FontWidth)
    {
        Result.XScale = SafeRatio1((float)(Result.TileCount*GlyphGen->FontWidth),
                                   (float)(Size.cx));
    }
    
    Result.YScale = 1.0f;
    if((uint32_t)Size.cy > GlyphGen->FontHeight)
    {
        Result.YScale = SafeRatio1((float)GlyphGen->FontHeight, (float)Size.cy);
    }
        
    return Result;
}

static void PrepareTilesForTransfer(glyph_generator *GlyphGen, d3d11_renderer *Renderer, size_t Count, wchar_t *String, glyph_dim Dim)
{
    DWORD StringLen = (DWORD)Count;
    Assert(StringLen == Count);

    DWriteDrawText(GlyphGen, StringLen, String, 0, 0, GlyphGen->TransferWidth, GlyphGen->TransferHeight,
                   Renderer->DWriteRenderTarget, Renderer->DWriteFillBrush, Dim.XScale, Dim.YScale);
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

    /* TODO(casey):

       At the moment, we do not do anything to fix the problem of trying to set the font size
       so large that it cannot be rasterized into the transfer buffer.  At some point, maybe
       we should warn about that and revert the font size to something smaller?
    */

    if(Renderer->DeviceContext)
    {
        glyph_cache_point Point = UnpackGlyphCachePoint(DestIndex);
        uint32_t X = Point.X*GlyphGen->FontWidth;
        uint32_t Y = Point.Y*GlyphGen->FontHeight;

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
}
