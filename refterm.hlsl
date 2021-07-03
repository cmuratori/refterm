struct TerminalCell
{
    uint GlyphIndex;
    uint Foreground;
    uint Background;
};

cbuffer ConstBuffer : register(b0)
{
    uint2 CellSize;
    uint2 TermSize;
    uint BlinkModulate;
    uint Pad0;
    uint Pad1;
    uint Pad2;
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

float4 ComputeOutputColor(uint2 ScreenPos)
{
    uint2 CellIndex = ScreenPos / CellSize;
    uint2 CellPos = ScreenPos % CellSize;

    TerminalCell Cell = Cells[CellIndex.y * TermSize.x + CellIndex.x];
    uint2 GlyphPos = UnpackGlyphXY(Cell.GlyphIndex)*CellSize;

    uint2 PixelPos = GlyphPos + CellPos;
    float4 GlyphTexel = GlyphTexture[PixelPos];

    float3 Background = UnpackColor(Cell.Background);
    float3 Foreground = UnpackColor(Cell.Foreground);
    float3 Blink = UnpackColor(BlinkModulate);

    float tBlink = float(Cell.Background >> 31);
    float3 Modulate = lerp(float3(1, 1, 1), Blink, tBlink);
    Foreground *= Modulate;

    // TODO: proper ClearType blending
    float3 Color = (1-GlyphTexel.a)*Background + GlyphTexel.rgb*Foreground;

    // NOTE(casey): Uncomment this to view the cache texture
    // Color = GlyphTexture[ScreenPos];

    return float4(Color, 1);
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
