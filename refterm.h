/* date = June 19th 2021 1:58 pm */

#ifndef REFTERM_H
#define REFTERM_H

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
    size_t LastAbsoluteP;
} ExampleParser;

typedef struct {
    size_t Offset; // NOTE(casey): Skip this many bytes of your append.

    size_t SizeA;
    char *DataA;

    size_t SizeB;
    char *DataB;
} SourceBufferRange;

typedef struct {
    size_t DataSize;
    char *Data;

    // NOTE(casey): For circular buffer
    size_t WrapPoint;

    // NOTE(casey): For cache checking
    size_t AbsoluteFilledSize;
} SourceBuffer;

typedef struct {
    glyph_hash RunHash;
    uint32_t Foreground;
    uint32_t Background;

    size_t AbsoluteP;
    uint32_t RunCount;
    uint32_t TileIndex;
} TerminalCell;

typedef struct {
    uint32_t DimX, DimY;
    TerminalCell *Cells;
} TerminalBuffer;

#endif //REFTERM_H
