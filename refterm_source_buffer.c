
static SourceBuffer AllocateSourceBuffer(size_t DataSize)
{
    SourceBuffer Result = {0};
    
    size_t SSEReadPad = 16;
    
    Result.Data = VirtualAlloc(0, DataSize + SSEReadPad, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if(Result.Data)
    {
        Result.DataSize = DataSize;
        Result.AbsoluteFilledSize = Result.DataSize; 
    }
    
    return Result;
}

static SourceBufferRange MakeSourceRange(SourceBuffer *Buffer, size_t RelP, size_t Size)
{
    SourceBufferRange Result = {0};
    
    size_t Last = (RelP + Size);
    if(Last <= Buffer->DataSize)
    {
        Result.SizeA = Size;
    }
    else
    {
        Result.SizeA = Last - RelP;
        Result.SizeB = Size;
        Result.DataB = Buffer->Data;
    }
    
    Result.DataA = Buffer->Data + RelP;
    
    return Result;
}

static int IsInBuffer(SourceBuffer *Buffer, size_t AbsoluteP)
{
    size_t BackwardOffset = Buffer->AbsoluteFilledSize - AbsoluteP;
    int Result = ((AbsoluteP < Buffer->AbsoluteFilledSize) &&
                  (BackwardOffset < Buffer->DataSize));
    return Result;
}

static SourceBufferRange ReadCharacters(SourceBuffer *Buffer, size_t AbsoluteP, size_t Size)
{
    SourceBufferRange Result = {0};
    if(IsInBuffer(Buffer, AbsoluteP))
    {
        size_t RelP = Buffer->DataSize - (Buffer->AbsoluteFilledSize - AbsoluteP);
        Result = MakeSourceRange(Buffer, RelP, Size);
    }
    return Result;
}

// TODO(casey): Eventually, when we know how the outer code
// works, we will not want to read from the source one
// character at a time.  We will probably want 128-bit reads?
static char ReadCharFromSource(SourceBuffer *Buffer, size_t AbsoluteP)
{
    char Result = 0;
    
    // TODO(casey): Test to make sure (Buffer->AbsoluteFilledSize == 0)
    // works here!
    
    // TODO(casey): Test both wrap point types, etc.
    
    if(AbsoluteP < Buffer->AbsoluteFilledSize)
    {
        size_t BackwardOffset = Buffer->AbsoluteFilledSize - AbsoluteP;
        if(BackwardOffset < Buffer->DataSize)
        {
            size_t Offset = Buffer->WrapPoint - BackwardOffset;
            if(BackwardOffset > Buffer->WrapPoint)
            {
                Offset += Buffer->DataSize;
            }
            Result = Buffer->Data[Offset];
        }
    }
    
    return Result;
}

static SourceBufferRange AppendToSource(SourceBuffer *Buffer, size_t AppendSize)
{
    SourceBufferRange Result = {0};
    
    if(AppendSize > Buffer->DataSize)
    {
        Buffer->WrapPoint = Buffer->DataSize;
        
        Result.Offset = AppendSize - Buffer->DataSize;
        Result.SizeA = Buffer->DataSize;
        Result.DataA = Buffer->Data;
    }
    else
    {
        Result = MakeSourceRange(Buffer, Buffer->WrapPoint, AppendSize);
        Buffer->WrapPoint += AppendSize;
        if(Buffer->WrapPoint > Buffer->DataSize)
        {
            Buffer->WrapPoint -= Buffer->DataSize;
        }
    }
    
    Buffer->AbsoluteFilledSize += AppendSize;
    
    return Result;
}

static SourceBufferRange GetLargestWriteRange(SourceBuffer *Buffer)
{
    Assert(Buffer->WrapPoint < Buffer->DataSize);
    
    SourceBufferRange Result = {0};
    Result.SizeA = Buffer->DataSize - Buffer->WrapPoint;
    Result.DataA = Buffer->Data + Buffer->WrapPoint;
    
    return Result;
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
static glyph_hash ComputeGlyphHash(SourceBuffer *Source, size_t AbsoluteP, size_t Count, char *Seedx16)
{
    // TODO(casey): Does the result of a grapheme composition
    // depend on whether or not it was RTL or LTR?  Or are there
    // no fonts that ever get used in both directions, so it doesn't
    // matter?
    
    // TODO(casey): Double-check exactly the pattern
    // we want to use for the hash here
    
    // TODO(casey): Crash on purpose!
    char unsigned *At = 0;
    
    Assert(IsInBuffer(Source, AbsoluteP));
    
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
    
    __m128i In = _mm_loadu_si128((__m128i *)At);
    In = _mm_and_si128(In, _mm_loadu_si128((__m128i *)(OverhangMask + 16 - Overhang)));
    
    HashValue = _mm_xor_si128(HashValue, In);
    HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
    HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
    HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
    HashValue = _mm_aesdec_si128(HashValue, _mm_setzero_si128());
    
    glyph_hash Result = {HashValue};
    return Result;
}
