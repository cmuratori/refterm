#ifndef Assert
#define Assert(...)
#define GLYPH_TABLE_UNDEF_ASSERT
#endif

#define DEBUG_VALIDATE_LRU 0

struct glyph_entry
{
    glyph_hash HashValue;

    uint32_t NextWithSameHash;
    uint32_t NextLRU;
    uint32_t PrevLRU;
    gpu_glyph_index GPUIndex;

    // NOTE(casey): For user use:
    uint32_t FilledState;
    uint32_t IndexCount;

#if DEBUG_VALIDATE_LRU
    size_t Ordering;
#endif
};

struct glyph_table
{
    glyph_table_stats Stats;

    uint32_t HashMask;
    uint32_t HashCount;
    uint32_t EntryCount;

    uint32_t *HashTable;
    glyph_entry *Entries;

#if DEBUG_VALIDATE_LRU
    uint32_t LastLRUCount;
#endif
};

static gpu_glyph_index PackGlyphCachePoint(uint32_t X, uint32_t Y)
{
    gpu_glyph_index Result = {(Y << 16) | X};
    return Result;
}

static glyph_cache_point UnpackGlyphCachePoint(gpu_glyph_index P)
{
    glyph_cache_point Result;

    Result.X = (P.Value & 0xffff);
    Result.Y = (P.Value >> 16);

    return Result;
}

static int GlyphHashesAreEqual(glyph_hash A, glyph_hash B)
{
    __m128i Compare = _mm_cmpeq_epi32(A.Value, B.Value);
    int Result = (_mm_movemask_epi8(Compare) == 0xffff);

    return Result;
}

static uint32_t *GetSlotPointer(glyph_table *Table, glyph_hash RunHash)
{
    uint32_t HashIndex = _mm_cvtsi128_si32(RunHash.Value);
    uint32_t HashSlot = (HashIndex & Table->HashMask);

    Assert(HashSlot < Table->HashCount);
    uint32_t *Result = &Table->HashTable[HashSlot];

    return Result;
}

static glyph_entry *GetEntry(glyph_table *Table, uint32_t Index)
{
    Assert(Index < Table->EntryCount);
    glyph_entry *Result = Table->Entries + Index;
    return Result;
}

static glyph_entry *GetSentinel(glyph_table *Table)
{
    glyph_entry *Result = Table->Entries;
    return Result;
}

static glyph_table_stats GetAndClearStats(glyph_table *Table)
{
    glyph_table_stats Result = Table->Stats;
    glyph_table_stats ZeroStats = {0};
    Table->Stats = ZeroStats;

    return Result;
}

static void UpdateGlyphCacheEntry(glyph_table *Table, uint32_t ID, uint32_t NewState, uint32_t NewIndexCount)
{
    glyph_entry *Entry = GetEntry(Table, ID);

    Entry->FilledState = NewState;
    Entry->IndexCount = NewIndexCount;
}

#if DEBUG_VALIDATE_LRU
static void ValidateLRU(glyph_table *Table, int ExpectedCountChange)
{
    uint32_t EntryCount = 0;

    glyph_entry *Sentinel = GetSentinel(Table);
    size_t LastOrdering = Sentinel->Ordering;
    for(uint32_t EntryIndex = Sentinel->NextLRU;
        EntryIndex != 0;
        )
    {
        glyph_entry *Entry = GetEntry(Table, EntryIndex);
        Assert(Entry->Ordering < LastOrdering);
        LastOrdering = Entry->Ordering;
        EntryIndex = Entry->NextLRU;
        ++EntryCount;
    }

    if((Table->LastLRUCount + ExpectedCountChange) != EntryCount)
    {
        __debugbreak();
    }
    Table->LastLRUCount = EntryCount;
}
#else
#define ValidateLRU(...)
#endif

static void RecycleLRU(glyph_table *Table)
{
    glyph_entry *Sentinel = GetSentinel(Table);

    // NOTE(casey): There are no more unused entries, evict the least recently used one
    Assert(Sentinel->PrevLRU);

    // NOTE(casey): Remove least recently used element from the LRU chain
    uint32_t EntryIndex = Sentinel->PrevLRU;
    glyph_entry *Entry = GetEntry(Table, EntryIndex);
    glyph_entry *Prev = GetEntry(Table, Entry->PrevLRU);
    Prev->NextLRU = 0;
    Sentinel->PrevLRU = Entry->PrevLRU;
    ValidateLRU(Table, -1);

    // NOTE(casey): Find the location of this entry in its hash chain
    uint32_t *NextIndex = GetSlotPointer(Table, Entry->HashValue);
    while(*NextIndex != EntryIndex)
    {
        Assert(*NextIndex);
        NextIndex = &GetEntry(Table, *NextIndex)->NextWithSameHash;
    }

    // NOTE(casey): Remove least recently used element from its hash chain, and place it on the free chain
    Assert(*NextIndex == EntryIndex);
    *NextIndex = Entry->NextWithSameHash;
    Entry->NextWithSameHash = Sentinel->NextWithSameHash;
    Sentinel->NextWithSameHash = EntryIndex;

    // NOTE(casey): Clear the index count and state
    UpdateGlyphCacheEntry(Table, EntryIndex, 0, 0);

    ++Table->Stats.RecycleCount;
}

static uint32_t PopFreeEntry(glyph_table *Table)
{
    glyph_entry *Sentinel = GetSentinel(Table);

    if(!Sentinel->NextWithSameHash)
    {
        RecycleLRU(Table);
    }

    uint32_t Result = Sentinel->NextWithSameHash;
    Assert(Result);

    // NOTE(casey): Pop this unused entry off the sentinel's chain of unused entries
    glyph_entry *Entry = GetEntry(Table, Result);
    Sentinel->NextWithSameHash = Entry->NextWithSameHash;
    Entry->NextWithSameHash = 0;

    Assert(Entry);
    Assert(Entry != Sentinel);
    Assert(Entry->IndexCount == 0);
    Assert(Entry->FilledState == 0);
    Assert(Entry->NextWithSameHash == 0);
    Assert(Entry == GetEntry(Table, Result));

    return Result;
}

static glyph_state FindGlyphEntryByHash(glyph_table *Table, glyph_hash RunHash)
{
    glyph_entry *Result = 0;

    uint32_t *Slot = GetSlotPointer(Table, RunHash);
    uint32_t EntryIndex = *Slot;
    while(EntryIndex)
    {
        glyph_entry *Entry = GetEntry(Table, EntryIndex);
        if(GlyphHashesAreEqual(Entry->HashValue, RunHash))
        {
            Result = Entry;
            break;
        }

        EntryIndex = Entry->NextWithSameHash;
    }

    if(Result)
    {
        Assert(EntryIndex);

        // NOTE(casey): An existing entry was found, remove it from the LRU
        glyph_entry *Prev = GetEntry(Table, Result->PrevLRU);
        glyph_entry *Next = GetEntry(Table, Result->NextLRU);

        Prev->NextLRU = Result->NextLRU;
        Next->PrevLRU = Result->PrevLRU;

        ValidateLRU(Table, -1);

        ++Table->Stats.HitCount;
    }
    else
    {
        // NOTE(casey): No existing entry was found, allocate a new one and link it into the hash chain

        EntryIndex = PopFreeEntry(Table);
        Assert(EntryIndex);

        Result = GetEntry(Table, EntryIndex);
        Assert(Result->FilledState == 0);
        Assert(Result->NextWithSameHash == 0);
        Assert(Result->IndexCount == 0);

        Result->NextWithSameHash = *Slot;
        Result->HashValue = RunHash;
        *Slot = EntryIndex;

        ++Table->Stats.MissCount;
    }

    // NOTE(casey): Update the LRU doubly-linked list to ensure this entry is now "first"
    glyph_entry *Sentinel = GetSentinel(Table);
    Assert(Result != Sentinel);
    Result->NextLRU = Sentinel->NextLRU;
    Result->PrevLRU = 0;

    glyph_entry *NextLRU = GetEntry(Table, Sentinel->NextLRU);
    NextLRU->PrevLRU = EntryIndex;
    Sentinel->NextLRU = EntryIndex;

#if DEBUG_VALIDATE_LRU
    Result->Ordering = Sentinel->Ordering++;
#endif
    ValidateLRU(Table, 1);

    glyph_state State;
    State.ID = EntryIndex;
    State.TileCount = Result->IndexCount;
    State.GPUIndex = Result->GPUIndex;
    State.FilledState = Result->FilledState;

    return State;
}

static void InitializeDirectGlyphTable(glyph_table_params Params, gpu_glyph_index *Table, int SkipZeroSlot)
{
    Assert(Params.CacheTileCountInX >= 1);
    
    if(SkipZeroSlot)
    {
        SkipZeroSlot = 1;
    }
        
    uint32_t X = SkipZeroSlot;
    uint32_t Y = 0;
    for(uint32_t EntryIndex = 0;
        EntryIndex < (Params.ReservedTileCount - SkipZeroSlot);
        ++EntryIndex)
    {
        if(X >= Params.CacheTileCountInX)
        {
            X = 0;
            ++Y;
        }

        Table[EntryIndex] = PackGlyphCachePoint(X, Y);

        ++X;
    }
}

static size_t GetGlyphTableFootprint(glyph_table_params Params)
{
    size_t HashSize = Params.HashCount*sizeof(uint32_t);
    size_t EntrySize = Params.EntryCount*sizeof(glyph_entry);
    size_t Result = (sizeof(glyph_table) + HashSize + EntrySize);

    return Result;
}

static glyph_table *PlaceGlyphTableInMemory(glyph_table_params Params, void *Memory)
{
    Assert(Params.HashCount >= 1);
    Assert(Params.EntryCount >= 2);
    Assert(IsPowerOfTwo(Params.HashCount));
    Assert(Params.CacheTileCountInX >= 1);

    glyph_table *Result = 0;

    if(Memory)
    {
        // NOTE(casey): Always put the glyph_entry array at the base of the memory, because the
        // compiler may generate aligned-SSE ops, which would crash if it was unaligned.
        glyph_entry *Entries = (glyph_entry *)Memory;
        Result = (glyph_table *)(Entries + Params.EntryCount);
        Result->HashTable = (uint32_t *)(Result + 1);
        Result->Entries = Entries;

        Result->HashMask = Params.HashCount - 1;
        Result->HashCount = Params.HashCount;
        Result->EntryCount = Params.EntryCount;

        memset(Result->HashTable, 0, Result->HashCount*sizeof(Result->HashTable[0]));

        uint32_t StartingTile = Params.ReservedTileCount;

        glyph_entry *Sentinel = GetSentinel(Result);
        uint32_t X = StartingTile % Params.CacheTileCountInX;
        uint32_t Y = StartingTile / Params.CacheTileCountInX;
        for(uint32_t EntryIndex = 0;
            EntryIndex < Params.EntryCount;
            ++EntryIndex)
        {
            if(X >= Params.CacheTileCountInX)
            {
                X = 0;
                ++Y;
            }

            glyph_entry *Entry = GetEntry(Result, EntryIndex);
            if((EntryIndex+1) < Params.EntryCount)
            {
                Entry->NextWithSameHash = EntryIndex + 1;
            }
            else
            {
                Entry->NextWithSameHash = 0;
            }
            Entry->GPUIndex = PackGlyphCachePoint(X, Y);

            Entry->FilledState = 0;
            Entry->IndexCount = 0;

            ++X;
        }

        GetAndClearStats(Result);
    }

    return Result;
}

#ifdef  GLYPH_TABLE_UNDEF_ASSERT
#undef Assert
#endif
