static glyph_generator AllocateGlyphGenerator(LPWSTR FontName, uint32_t FontHeight,
                                              ID3D11Texture2D *GlyphTexture)
{
    glyph_generator GlyphGen = {0};

    GlyphGen.FontHeight = FontHeight;

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

    GlyphGen.Pixels = (uint32_t *)Pixels;
    GlyphGen.Pitch = REFTERM_TEXTURE_WIDTH * 4; // RGBA bitmap

    HFONT Font = CreateFontW(FontHeight, 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                             ClearType ? CLEARTYPE_QUALITY : ANTIALIASED_QUALITY, FIXED_PITCH, FontName);
    Assert(Font);
    SelectObject(DC, Font);

    TEXTMETRICW Metrics;
    BOOL ok = GetTextMetricsW(DC, &Metrics);
    Assert(ok);

    GlyphGen.FontWidth = Metrics.tmAveCharWidth + 1; // not sure why +1 is needed here

    SetTextColor(DC, RGB(255, 255, 255));
    SetBkColor(DC, RGB(0, 0, 0));

    return GlyphGen;
}

static GlyphEntry *GetOrFillCache(glyph_generator *GlyphGen, ID3D11DeviceContext* DeviceContext, SourceBuffer *Source, GlyphTable *Table, size_t AbsoluteP, size_t RunCount, glyph_hash RunHash, size_t FrameIndex)
{
    GlyphLookup Lookup = FindGlyphEntryByHash(Table, RunHash, FrameIndex);
    if(!Lookup.Found)
    {
        // TODO(casey): Windows can only 2^31 glyph runs - which
        // seems fine, but... technically Unicode can have more than two
        // billion combining characters???
        Assert((RunCount % 2) == 0);
        int StringLen = (int)(RunCount / 2);

        SourceBufferRange ReadRange = ReadCharacters(Source, AbsoluteP, RunCount);
        if(ReadRange.SizeA)
        {
            WCHAR *String = 0;
            if(ReadRange.SizeB)
            {
                // TODO(casey): Do the wrap gather here
                Assert(!"Wrap unsupported!!!");
            }
            else
            {
                String = (WCHAR *)ReadRange.DataA;
            }

            RECT Rect = {0, 0, GlyphGen->FontWidth, GlyphGen->FontHeight};
            ExtTextOutW(GlyphGen->DC, Rect.left, Rect.top, ETO_OPAQUE, 0, String, StringLen, 0);

            SIZE Size;
            GetTextExtentPointW(GlyphGen->DC, String, StringLen, &Size);

            uint16_t NewIndexCount =
                (uint16_t)((Size.cx + GlyphGen->FontWidth - 1) / GlyphGen->FontWidth);

            // TODO(casey): How do we want to do DirectX batching here?
            // This should build up batches and then send them, but
            // I'm not sure how those unpacks should work.  We want
            // to batch them across many cache fills so that we can
            // do single downloads of the entire frames' new data,
            // but I'm not sure how to do the scatter...

            GPUGlyphIndex *DestIndex = UpdateCacheEntry(Table, Lookup.Entry, RunHash, NewIndexCount, FrameIndex);
            uint32_t *SourceCorner = GlyphGen->Pixels;
            int32_t dSourceCorner = GlyphGen->FontWidth;
            for(uint16_t IndexIndex = 0;
                IndexIndex < NewIndexCount;
                ++IndexIndex)
            {
                glyph_cache_point Point = UnpackCachePoint(DestIndex[IndexIndex]);
                D3D11_BOX TexelBox = {
                    .left = Point.X*GlyphGen->FontWidth,
                    .right = (Point.X + 1)*GlyphGen->FontWidth,
                    .top = Point.Y*GlyphGen->FontHeight,
                    .bottom = (Point.Y + 1)*GlyphGen->FontHeight,
                    .front = 0,
                    .back = 1,
                };
                
                SourceCorner += dSourceCorner;
                ID3D11DeviceContext_UpdateSubresource(DeviceContext, (ID3D11Resource*)GlyphGen->GlyphTexture, 0, &TexelBox, SourceCorner, GlyphGen->Pitch, 0);
            }
        }
        else
        {
            // TODO(casey): Decide what we want to render
            // when the user scrolls back to a position, or
            // there is sufficient overdraw, that we no
            // longer have the data necessary to produce
            // a cell.  We probably want something here
            // that uses the data from the previous frame,
            // so we should put that in the shader and do it.
            Assert(!"No data available");
        }
    }

    GlyphEntry *Result = Lookup.Entry;
    return Result;
}
