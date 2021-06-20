/* date = June 19th 2021 10:47 pm */

#ifndef REFTERM_GLYPH_GENERATOR_H
#define REFTERM_GLYPH_GENERATOR_H

typedef struct
{
    HDC DC;
    uint32_t FontWidth, FontHeight;
    uint32_t Pitch;
    uint32_t *Pixels;
    
    ID3D11Texture2D *GlyphTexture;

    // TODO(casey): This should be easy to turn on, when we're ready!
    // int RightToLeft;
} glyph_generator;

#endif //REFTERM_GLYPH_GENERATOR_H
