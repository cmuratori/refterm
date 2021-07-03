/* NOTE(casey): 

   There is no reason for this file to exist at all, other than the fact that
   they didn't bother to make DirectWrite work with C.  So this file just
   hides the DirectWrite C++ calls behind a simple C API so that they can
   be used by people writing plain C code.
*/

typedef struct ID2D1RenderTarget ID2D1RenderTarget;
typedef struct ID2D1SolidColorBrush ID2D1SolidColorBrush;
int D2DAcquire(IDXGISurface *GlyphTransferSurface,
               ID2D1RenderTarget **DWriteRenderTarget,
               ID2D1SolidColorBrush **DWriteFillBrush);
void D2DRelease(ID2D1RenderTarget **DWriteRenderTarget,
                ID2D1SolidColorBrush **DWriteFillBrush);

typedef struct glyph_generator glyph_generator;
int DWriteInit(glyph_generator *GlyphGen, IDXGISurface *GlyphTransferSurface);

int DWriteSetFont(glyph_generator *GlyphGen, wchar_t *FontName, uint32_t FontHeight);

void DWriteDrawText(glyph_generator *GlyphGen, int StringLen, wchar_t *String,
                    uint32_t Left, uint32_t Top, uint32_t Right, uint32_t Bottom,
                    ID2D1RenderTarget *RenderTarget,
                    ID2D1SolidColorBrush *FillBrush);

SIZE DWriteGetTextExtent(glyph_generator *GlyphGen, int StringLen, wchar_t *String);

void DWriteRelease(glyph_generator *GlyphGen);