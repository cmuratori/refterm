/* date = June 20th 2021 0:25 am */

#ifndef REFTERM_CACHE_H
#define REFTERM_CACHE_H

typedef struct {
    __m128i Value;
} glyph_hash;

// TODO(casey): Can anyone verify the maximum number
// of tiles that a single Unicode grapheme can occupy?
#define MAX_TILES_PER_GRAPHEME 16
typedef uint32_t GPUGlyphIndex;
typedef struct {
    glyph_hash HashValue;
    size_t FrameIndex; // NOTE(casey): 0 when unfilled
    // TODO(casey): This alignment is bad... think about
    // the best way to pack this, and possibly split these
    // into two structures? Possibly use zero-termination
    // for graphemes?
    uint16_t IndexCount; // NOTE(casey): 0 when unfilled
    GPUGlyphIndex GPUIndexes[MAX_TILES_PER_GRAPHEME]; // 16*4
} GlyphEntry;

typedef struct {
    uint32_t HashCount;
    uint32_t AssocCount;
    uint32_t IndexCount;
} GlyphTableParams;

typedef struct {
    uint32_t HashMask;
    uint32_t AssocShift;
    
    uint32_t HashCount;
    uint32_t AssocCount;
    
    GlyphEntry *Hash;
    
    uint32_t MaxGPUIndexCount;
    uint32_t FreeGPUIndexCount;
    GPUGlyphIndex *GPUIndexes;
} GlyphTable;

typedef struct {
    GlyphEntry *Entry;
    int Found;
} GlyphLookup;

typedef struct {
    uint32_t X, Y;
} glyph_cache_point;

#endif //REFTERM_CACHE_H
