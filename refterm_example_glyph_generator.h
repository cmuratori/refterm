typedef enum
{
    GlyphState_None,
    GlyphState_Sized,
    GlyphState_Rasterized,
} glyph_entry_state;

typedef enum
{
    GlyphGen_UseClearType = 0x1, // TODO(casey): This is not yet supported in the DirectWrite path
    GlyphGen_UseDirectWrite = 0x2,
} glyph_generation_flag;

typedef struct glyph_generator glyph_generator;

struct glyph_generator
{
    uint32_t FontWidth, FontHeight;
    uint32_t Pitch;
    uint32_t *Pixels;
    
    uint32_t TransferWidth;
    uint32_t TransferHeight;
    
    uint32_t UseClearType;
    uint32_t UseDWrite;

    // NOTE(casey): For GDI-based generation:
    HDC DC;
    HFONT OldFont, Font;
    HBITMAP Bitmap;
    
    // NOTE(casey): For DWrite-based generation:
    struct IDWriteFactory *DWriteFactory;
    struct IDWriteFontFace *FontFace;
    struct IDWriteTextFormat *TextFormat;
};
