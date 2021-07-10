struct TerminalCell
{
    uint GlyphIndex;
    uint Foreground;
    uint Background;
};

cbuffer ConstBuffer : register(b0)
{
    uint2 CellSize;
    uint2 GlyphSize;
    uint2 TermSize;
    uint2 Pad;
    uint2 TopLeftMargin;
    uint BlinkModulate;
    uint MarginColor;
};

StructuredBuffer<TerminalCell> Cells : register(t0);
Texture2D<float4> GlyphTexture : register(t1);

float3 UnpackColor(uint Packed)
{
    int R = Packed & 0xff;
    int G = (Packed >> 8) & 0xff;
    int B = (Packed >> 16) & 0xff;

    return float3(R, G, B) / 255.0;
}

uint2 UnpackGlyphXY(uint GlyphIndex)
{
    int x = (GlyphIndex & 0xffff);
    int y = (GlyphIndex >> 16);
    return uint2(x, y);
}

float4 SampleGlyph(uint2 CellIndex, uint2 BaseCellPos, int dX, int dY)
{
    // TODO(casey): Is the compiler smart enough not to do this 4 times?
    float3 Blink = UnpackColor(BlinkModulate);

    float4 Texel = float4(0, 0, 0, 0);

    uint2 CellPos = BaseCellPos - dX*CellSize.x - dY*CellSize.y;
    if((CellPos.x < GlyphSize.x) &&
       (CellPos.y < GlyphSize.y))
    {
        TerminalCell Cell = Cells[(CellIndex.y + dY) * TermSize.x + (CellIndex.x + dX)];
        uint2 GlyphPos = UnpackGlyphXY(Cell.GlyphIndex)*GlyphSize;
        Texel = GlyphTexture[GlyphPos + CellPos];

        float3 Foreground = UnpackColor(Cell.Foreground);

        // TODO(casey): Pack blink into foreground instead, so we never read background of neighbors
        float tBlink = float(Cell.Background >> 31);
        float3 Modulate = lerp(float3(1, 1, 1), Blink, tBlink);
        Foreground *= Modulate;

        Texel.rgb *= Foreground;
    }

    return Texel;
}

float4 ComputeOutputColor(uint2 ScreenPos)
{
    uint2 CellIndex = (ScreenPos - TopLeftMargin) / CellSize;
    uint2 CellPos = (ScreenPos - TopLeftMargin) % CellSize;

    float3 Result;

    if((ScreenPos.x >= TopLeftMargin.x) &&
       (ScreenPos.y >= TopLeftMargin.y) &&
       (CellIndex.x < TermSize.x) &&
       (CellIndex.y < TermSize.y))
    {
        // TODO(casey): Should I try to coallesce this with SampleGlyph?
        TerminalCell Cell = Cells[CellIndex.y*TermSize.x + CellIndex.x];

        float4 Left = SampleGlyph(CellIndex, CellPos, -1, 0);
        float4 Top = SampleGlyph(CellIndex, CellPos, 0, -1);
        float4 TopLeft = SampleGlyph(CellIndex, CellPos, -1, -1);
        float4 Center = SampleGlyph(CellIndex, CellPos, 0, 0);

        float4 Foreground = max(max(Left, Top), max(TopLeft, Center));
        float3 Background = UnpackColor(Cell.Background);

        Result = (1-Foreground.a)*Background + Foreground.rgb;
    }
    else
    {
        Result = UnpackColor(MarginColor);
    }

#if 0
    // NOTE(casey): Turn this on to see the glyph cache texture
    Result = GlyphTexture[ScreenPos].rgb;
    if((ScreenPos.x / GlyphSize.x) % 2)
    {
        Result.rgb += float3(.1, .1, .1);
    }
    if((ScreenPos.y / GlyphSize.y) % 2)
    {
        Result.rgb += float3(.1, .1, .1);
    }
#endif

    return float4(Result, 1);
}

//
// NOTE(casey): This is the pixel shader version
//

float4 VertexMain(uint vI : SV_VERTEXID):SV_POSITION
{
    return float4(2.0*(float(vI&1) - 0.5), -(float(vI>>1) - 0.5)*2.0, 0, 1);
}

float4 PixelMain(float4 ScreenPos:SV_POSITION):SV_TARGET
{
    return ComputeOutputColor(ScreenPos.xy);
}

//
// NOTE(casey): This is the compute shader version
//

RWTexture2D<float4> Output : register(u0);

// dispatch with (TermSize*CellSize+7)/8 groups for x,y and 1 for z
[numthreads(8, 8, 1)]
void ComputeMain(uint3 Id: SV_DispatchThreadID)
{
    uint2 ScreenPos = Id.xy;
    Output[ScreenPos] = ComputeOutputColor(ScreenPos);
}
