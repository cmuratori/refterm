// TODO(casey): Need to battle-test everything here
// with extensive testing, so do not use until we
// have done so.

static uint32_t PackCachePoint(uint32_t X, uint32_t Y)
{
    uint32_t Result = ((Y << 16) | X);
    return Result;
}

static glyph_cache_point UnpackCachePoint(uint32_t P)
{
    glyph_cache_point Result;

    Result.X = (P & 0xffff);
    Result.Y = (P >> 16);

    return Result;
}

static void PushFreeIndex(GlyphTable *Table, GPUGlyphIndex Index)
{
    Assert(Table->FreeGPUIndexCount < Table->MaxGPUIndexCount);
    Table->GPUIndexes[Table->FreeGPUIndexCount++] = Index;
}

static GPUGlyphIndex PopFreeIndex(GlyphTable *Table)
{
    while(Table->FreeGPUIndexCount == 0)
    {
        // TODO(casey): Evict "randomly" here
        Assert(!"Implement choice-of-two freeing");
    }

    Assert(Table->FreeGPUIndexCount);
    GPUGlyphIndex Result = Table->GPUIndexes[--Table->FreeGPUIndexCount];

    return Result;
}

static GPUGlyphIndex *UpdateCacheEntry(GlyphTable *Table, GlyphEntry *Entry, glyph_hash Hash, uint16_t NewIndexCount, size_t FrameIndex)
{
    Assert(NewIndexCount < MAX_TILES_PER_GRAPHEME);
    while(Entry->IndexCount > NewIndexCount)
    {
        PushFreeIndex(Table, Entry->GPUIndexes[--Entry->IndexCount]);
    }

    while(Entry->IndexCount < NewIndexCount)
    {
        Entry->GPUIndexes[Entry->IndexCount++] = PopFreeIndex(Table);
    }

    Entry->HashValue = Hash;
    Entry->FrameIndex = FrameIndex;

    GPUGlyphIndex *Result = Entry->GPUIndexes;
    return Result;
}

static void EvictEntry(GlyphTable *Table, GlyphEntry *Entry)
{
    glyph_hash NullHash = {0};
    UpdateCacheEntry(Table, Entry, NullHash, 0, 0);
}

static int AreEqual(glyph_hash A, glyph_hash B)
{
    // TODO(casey): Double-check this compare
    __m128i Compare = _mm_cmpeq_epi32(A.Value, B.Value);
    int Result = (_mm_movemask_epi8(Compare) == 0xf);

    return Result;
}

static GlyphLookup FindGlyphEntryByHash(GlyphTable *Table, glyph_hash RunHash, size_t FrameIndex)
{
    GlyphLookup Result = {0};

    int Match = 0;
    uint32_t HashIndex = _mm_cvtsi128_si32(RunHash.Value);
    GlyphEntry *Slot = Table->Hash + ((HashIndex & Table->HashMask) << Table->AssocShift);
    Result.Entry = Slot;
    for(uint32_t SlotOffset = 0; SlotOffset < Table->AssocCount; ++SlotOffset, ++Slot)
    {
        if(Slot->FrameIndex && AreEqual(Slot->HashValue, RunHash))
        {
            Slot->FrameIndex = FrameIndex;
            Result.Entry = Slot;
            Result.Found = 1;
            break;
        }

        if(Result.Entry->FrameIndex > Slot->FrameIndex)
        {
            Result.Entry = Slot;
        }
    }

    return Result;
}

static GlyphTable AllocateGlyphTable(GlyphTableParams Params, uint32_t XCacheCount)
{
    Assert(IsPowerOfTwo(Params.HashCount));
    Assert(IsPowerOfTwo(Params.AssocCount));

    GlyphTable Result = {0};

    size_t EntryCount = Params.HashCount*Params.AssocCount;

    size_t EntrySize = EntryCount*sizeof(GlyphEntry);
    size_t IndexSize = Params.IndexCount*sizeof(GPUGlyphIndex);
    size_t TotalSize = (EntrySize + IndexSize);

    char *Memory = (char *)VirtualAlloc(0, TotalSize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if(Memory)
    {
        Result.Hash = (GlyphEntry *)Memory;
        Result.GPUIndexes = (GPUGlyphIndex *)(Memory + EntrySize);

        Result.HashMask = Params.HashCount - 1;

        // TODO(casey): More clever shift deduction here?
        for(int AssocIndex = Params.AssocCount;
            AssocIndex > 1;
            AssocIndex /= 2)
        {
            ++Result.AssocShift;
        }

        Result.HashCount = Params.HashCount;
        Result.AssocCount = Params.AssocCount;

        Result.MaxGPUIndexCount = Params.IndexCount;
        Result.FreeGPUIndexCount = Params.IndexCount;

        uint32_t X = 0;
        uint32_t Y = 0;
        for(uint32_t IndexIndex = 0;
            IndexIndex < Params.IndexCount;
            ++IndexIndex)
        {
            Result.GPUIndexes[IndexIndex] = PackCachePoint(X, Y);
            ++X;
            if(X >= XCacheCount)
            {
                X = 0;
                ++Y;
            }
        }
    }

    return Result;
}