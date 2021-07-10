static source_buffer AllocateSourceBuffer(size_t DataSize)
{
    source_buffer Result = {0};
    
    SYSTEM_INFO Info;
    GetSystemInfo(&Info);
    Assert(IsPowerOfTwo(Info.dwAllocationGranularity));
    
    // NOTE(casey): This has to be aligned to the allocation granularity otherwise the back-to-back buffer mapping might
    // not work.
    DataSize = (DataSize + Info.dwAllocationGranularity - 1) & ~(Info.dwAllocationGranularity - 1);
    HANDLE Section = CreateFileMapping (INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, (DWORD)(DataSize >> 32), (DWORD)(DataSize & 0xffffffff), 0);
    
 #ifdef MEM_REPLACE_PLACEHOLDER
    void* Placeholder1 = VirtualAlloc2 (0, 0,
                                        2 * DataSize, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                                        0, 0);
    VirtualFree (Placeholder1, DataSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    void *Placeholder2 = ((char *)Placeholder1 + DataSize);
    
    void *View1 = MapViewOfFile3 (Section, 0, Placeholder1, 0, DataSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, 0, 0);
    void *View2 = MapViewOfFile3 (Section, 0, Placeholder2, 0, DataSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, 0, 0);
    if(View1 && View2)
    {
        Result.Data = View1;
        Result.DataSize = DataSize;
    }
    else
    {
        MessageBoxW(0, L"Unable to allocate scrollback buffer with placeholder", L"WARNING", MB_OK);
#endif
        for(size_t Offset = 0x40000000;
            Offset < 0x400000000;
            Offset += 0x1000000)
        {
            // TODO(casey): Harden this path to try multiple times
            void *View1 = (char *)MapViewOfFileEx(Section, FILE_MAP_ALL_ACCESS, 0, 0, DataSize, (void *)Offset);
            void *View2 = MapViewOfFileEx(Section, FILE_MAP_ALL_ACCESS, 0, 0, DataSize, ((char *)View1 + DataSize));
            
            if(View1 && View2)
            {
                Result.Data = View1;
                Result.DataSize = DataSize;
                break;
            }
            
            if(View1) UnmapViewOfFile(View1);
            if(View2) UnmapViewOfFile(View2);
        }
        
        if(!Result.Data)
        {
            MessageBoxW(0, L"Unable to allocate scrollback buffer with probing", L"Fatal error", MB_OK|MB_ICONSTOP);
        }
#ifdef MEM_REPLACE_PLACEHOLDER
    }
#endif
    
    return Result;
}

static int IsInBuffer(source_buffer *Buffer, size_t AbsoluteP)
{
    size_t BackwardOffset = Buffer->AbsoluteFilledSize - AbsoluteP;
    int Result = ((AbsoluteP < Buffer->AbsoluteFilledSize) &&
                  (BackwardOffset < Buffer->DataSize));
    return Result;
}

static source_buffer_range AdvanceRange(source_buffer_range Source, size_t ToAbsoluteP, size_t Count)
{
    source_buffer_range Result = Source;
    
    // NOTE(casey): Moving ranges backwards isn't safe, because you may slide off the beginning of the circular buffer.
    Assert(ToAbsoluteP >= Result.AbsoluteP);
    
    Result.Data += ToAbsoluteP - Result.AbsoluteP;
    Result.AbsoluteP = ToAbsoluteP;
    Result.Count = Count;
    
    return Result;
}

static source_buffer_range ConsumeCount(source_buffer_range Source, size_t Count)
{
    source_buffer_range Result = Source;
    
    if(Count > Result.Count)
    {
        Count = Result.Count;
    }
    
    Result.Data += Count;
    Result.AbsoluteP += Count;
    Result.Count -= Count;
    
    return Result;
}

static source_buffer_range ReadSourceAt(source_buffer *Buffer, size_t AbsoluteP, size_t Count)
{
    source_buffer_range Result = {0};
    if(IsInBuffer(Buffer, AbsoluteP))
    {
        Result.AbsoluteP = AbsoluteP;
        Result.Count = (Buffer->AbsoluteFilledSize - AbsoluteP);
        Result.Data = Buffer->Data + Buffer->DataSize + Buffer->RelativePoint - Result.Count;
        
        if(Result.Count > Count)
        {
            Result.Count = Count;
        }
    }
    
    return Result;
}

static size_t GetCurrentAbsoluteP(source_buffer *Buffer)
{
    size_t Result = Buffer->AbsoluteFilledSize;
    return Result;
}

#define LARGEST_AVAILABLE ((size_t)-1)
static source_buffer_range GetNextWritableRange(source_buffer *Buffer, size_t MaxCount)
{
    Assert(Buffer->RelativePoint < Buffer->DataSize);
    
    source_buffer_range Result = {0};
    Result.AbsoluteP = Buffer->AbsoluteFilledSize;
    Result.Count = Buffer->DataSize;
    Result.Data = Buffer->Data + Buffer->RelativePoint;

    if(Result.Count > MaxCount)
    {
        Result.Count = MaxCount;
    }
    
    return Result;
}

static void CommitWrite(source_buffer *Buffer, size_t Size)
{
    Assert(Buffer->RelativePoint < Buffer->DataSize);
    Assert(Size <= Buffer->DataSize);
    
    Buffer->RelativePoint += Size;
    Buffer->AbsoluteFilledSize += Size;
    
    size_t WrappedRelative = Buffer->RelativePoint - Buffer->DataSize;
    Buffer->RelativePoint = (Buffer->RelativePoint >= Buffer->DataSize) ? WrappedRelative : Buffer->RelativePoint;
    
    Assert(Buffer->RelativePoint < Buffer->DataSize);
}

static char unsigned OverhangMask[32] =
{
    255, 255, 255, 255,  255, 255, 255, 255,  255, 255, 255, 255,  255, 255, 255, 255,
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0
};
static char unsigned DefaultSeed[16] =
{
    178, 201, 95, 240, 40, 41, 143, 216,
    2, 209, 178, 114, 232, 4, 176, 188
};
static glyph_hash ComputeGlyphHash(size_t Count, char unsigned *At, char unsigned *Seedx16)
{
    /* TODO(casey):
    
      Consider and test some alternate hash designs.  The hash here
      was the simplest thing to type in, but it is not necessarily
      the best hash for the job.  It may be that less AES rounds 
      would produce equivalently collision-free results for the
      problem space.  It may be that non-AES hashing would be
      better.  Some careful analysis would be nice.
    */
      
    // TODO(casey): Does the result of a grapheme composition
    // depend on whether or not it was RTL or LTR?  Or are there
    // no fonts that ever get used in both directions, so it doesn't
    // matter?
    
    // TODO(casey): Double-check exactly the pattern
    // we want to use for the hash here
    
    glyph_hash Result = {0};
    
    // TODO(casey): Should there be an IV?
    __m128i HashValue = _mm_cvtsi64_si128(Count);
    HashValue = _mm_xor_si128(HashValue, _mm_loadu_si128((__m128i *)Seedx16));
    
    size_t ChunkCount = Count / 16;
    while(ChunkCount--)
    {
        __m128i In = _mm_loadu_si128((__m128i *)At);
        
        HashValue = _mm_xor_si128(HashValue, In);
        HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
        HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
        HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
        HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
    }
    
    size_t Overhang = Count % 16;
    
    
#if 0
    __m128i In = _mm_loadu_si128((__m128i *)At);
#else
    // TODO(casey): This needs to be improved - it's too slow, and the #if 0 branch would be nice but can't
    // work because of overrun, etc.
    char Temp[16];
    __movsb((unsigned char *)Temp, At, Overhang);
    __m128i In = _mm_loadu_si128((__m128i *)Temp);
#endif
    In = _mm_and_si128(In, _mm_loadu_si128((__m128i *)(OverhangMask + 16 - Overhang)));
    HashValue = _mm_xor_si128(HashValue, In);
    HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
    HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
    HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
    HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
    
    Result.Value = HashValue;
    
    return Result;
}

static glyph_hash ComputeHashForTileIndex(glyph_hash Tile0Hash, uint32_t TileIndex)
{
    __m128i HashValue = Tile0Hash.Value;
    if(TileIndex)
    {
        HashValue = _mm_xor_si128(HashValue, _mm_set1_epi32(TileIndex));
        HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
        HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
        HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
        HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
    }
    
    glyph_hash Result = {HashValue};
    return Result;
}
    