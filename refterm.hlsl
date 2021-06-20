//

struct TerminalCell
{
    uint GlyphIndex; // index into GlyphMapping buffer
    uint Foreground;
    uint Background;
};

cbuffer ConstBuffer : register(b0)
{
    uint2 CellSize;
    uint2 TermSize;
}; 

// TermSize.x * TermSize.y amount of cells to render as output
StructuredBuffer<TerminalCell> Cells : register(t0);

Texture2D<float3> GlyphTexture : register(t1);

RWTexture2D<float4> Output : register(u0);

float3 GetColor(uint i)
{
    int r = i & 0xff;
    int g = (i >> 8) & 0xff;
    int b = (i >> 16) & 0xff;
    return float3(r, g, b) / 255.0;
}

uint2 UnpackGlyphXY(uint GlyphIndex)
{
    int x = (GlyphIndex & 0xffff);
    int y = (GlyphIndex >> 16);
    return uint2(x, y);
}

// dispatch with (TermSize*CellSize+7)/8 groups for x,y and 1 for z
[numthreads(8, 8, 1)]
void shader(uint3 Id: SV_DispatchThreadID)
{
    uint2 ScreenPos = Id.xy;

    uint2 CellIndex = ScreenPos / CellSize;
    uint2 CellPos = ScreenPos % CellSize;

    TerminalCell Cell = Cells[CellIndex.y * TermSize.x + CellIndex.x];
    uint2 GlyphPos = UnpackGlyphXY(Cell.GlyphIndex)*CellSize;

    uint2 PixelPos = GlyphPos + CellPos;
    float3 Alpha = GlyphTexture[PixelPos];

    float3 Background = GetColor(Cell.Background);
    float3 Foreground = GetColor(Cell.Foreground);

    // TODO: proper ClearType blending
    float3 Color = lerp(Background, Foreground, Alpha);

    Output[ScreenPos] = float4(Color, 1);
}
