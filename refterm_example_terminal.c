static terminal_buffer AllocateTerminalBuffer(int DimX, int DimY)
{
    terminal_buffer Result = {0};

    size_t TotalSize = sizeof(renderer_cell)*DimX*DimY;
    Result.Cells = VirtualAlloc(0, TotalSize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if(Result.Cells)
    {
        Result.DimX = DimX;
        Result.DimY = DimY;
    }

    return Result;
}

static void DeallocateTerminalBuffer(terminal_buffer *Buffer)
{
    if(Buffer && Buffer->Cells)
    {
        VirtualFree(Buffer->Cells, 0, MEM_RELEASE);
        Buffer->DimX = Buffer->DimY = 0;
        Buffer->Cells = 0;
    }
}

static DWORD GetPipePendingDataCount(HANDLE Pipe)
{
    DWORD Result = 0;
    PeekNamedPipe(Pipe, 0, 0, 0, &Result, 0);

    return Result;
}

static void UpdateLineEnd(example_terminal *Terminal, size_t ToP)
{
    Terminal->Lines[Terminal->CurrentLineIndex].OnePastLastP = ToP;
}

static void LineFeed(example_terminal *Terminal, size_t AtP, size_t NextLineStart, glyph_props AtProps)
{
    UpdateLineEnd(Terminal, AtP);
    ++Terminal->CurrentLineIndex;
    if(Terminal->CurrentLineIndex >= Terminal->MaxLineCount)
    {
        Terminal->CurrentLineIndex = 0;
    }

    example_line *Line = Terminal->Lines + Terminal->CurrentLineIndex;
    Line->FirstP = NextLineStart;
    Line->OnePastLastP = NextLineStart;
    Line->ContainsComplexChars = 0;
    Line->StartingProps = AtProps;

    if(Terminal->LineCount <= Terminal->CurrentLineIndex)
    {
        Terminal->LineCount = Terminal->CurrentLineIndex + 1;
    }
}

static int IsInBounds(terminal_buffer *Buffer, terminal_point Point)
{
    int Result = ((Point.X >= 0) && (Point.X < (int)Buffer->DimX) &&
                  (Point.Y >= 0) && (Point.Y < (int)Buffer->DimY));
    return Result;
}

static uint32_t PackRGB(uint32_t R, uint32_t G, uint32_t B)
{
    if(R > 255) R = 255;
    if(G > 255) G = 255;
    if(B > 255) B = 255;
    uint32_t Result = ((B << 16) | (G << 8) | (R << 0));
    return Result;
}

static int IsDigit(char Digit)
{
    int Result = ((Digit >= '0') && (Digit <= '9'));
    return Result;
}

static char PeekToken(source_buffer_range *Range, int Ordinal)
{
    char Result = 0;

    if(Ordinal < Range->Count)
    {
        Result = Range->Data[Ordinal];
    }

    return Result;
}

static char GetToken(source_buffer_range *Range)
{
    char Result = 0;

    if(Range->Count)
    {
        Result = Range->Data[0];
        *Range = ConsumeCount(*Range, 1);
    }

    return Result;
}

static uint32_t ParseNumber(source_buffer_range *Range)
{
    uint32_t Result = 0;
    while(IsDigit(PeekToken(Range, 0)))
    {
        char Token = GetToken(Range);
        Result = 10*Result + (Token - '0');
    }
    return Result;
}

static int AtEscape(source_buffer_range *Range)
{
    int Result = ((PeekToken(Range, 0) == '\x1b') &&
                  (PeekToken(Range, 1) == '['));
    return Result;
}

static int IsDirectCodepoint(wchar_t CodePoint)
{
    int Result = ((CodePoint >= MinDirectCodepoint) &&
                  (CodePoint <= MaxDirectCodepoint));
    return Result;
}

static renderer_cell *GetCell(terminal_buffer *Buffer, terminal_point Point)
{
    renderer_cell *Result = IsInBounds(Buffer, Point) ? (Buffer->Cells + Point.Y*Buffer->DimX + Point.X) : 0;
    return Result;
}

static void ClearCellCount(example_terminal *Terminal, int32_t Count, renderer_cell *Cell)
{
    uint32_t Background = Terminal->DefaultBackgroundColor;
    while(Count--)
    {
        Cell->GlyphIndex = 0;
        Cell->Foreground = Background; // TODO(casey): Should be able to set this to 0, but need to make sure cache slot 0 never gets filled
        Cell->Background = Background;
        ++Cell;
    }
}

static void ClearLine(example_terminal *Terminal, terminal_buffer *Buffer, int32_t Y)
{
    terminal_point Point = {0, Y};
    if(IsInBounds(Buffer, Point))
    {
        ClearCellCount(Terminal, Buffer->DimX, GetCell(Buffer, Point));
    }
}

static void Clear(example_terminal *Terminal, terminal_buffer *Buffer)
{
    ClearCellCount(Terminal, Buffer->DimX*Buffer->DimY, Buffer->Cells);
}

static void AdvanceRowNoClear(example_terminal *Terminal, terminal_point *Point)
{
    Point->X = 0;
    ++Point->Y;
    if(Point->Y >= (int32_t)Terminal->ScreenBuffer.DimY)
    {
        Point->Y = 0;
    }
}

static void AdvanceRow(example_terminal *Terminal, terminal_point *Point)
{
    AdvanceRowNoClear(Terminal, Point);
    ClearLine(Terminal, &Terminal->ScreenBuffer, Point->Y);
}

static void AdvanceColumn(example_terminal *Terminal, terminal_point *Point)
{
    ++Point->X;
    if(Terminal->LineWrap && (Point->X >= (int32_t)Terminal->ScreenBuffer.DimX))
    {
        AdvanceRow(Terminal, Point);
    }
}

static void SetCellDirect(gpu_glyph_index GPUIndex, glyph_props Props, renderer_cell *Dest)
{
    Dest->GlyphIndex = GPUIndex.Value;
    uint32_t Foreground = Props.Foreground;
    uint32_t Background = Props.Background;
    if(Props.Flags & TerminalCell_ReverseVideo)
    {
        Foreground = Props.Background;
        Background = Props.Foreground;
    }

    if(Props.Flags & TerminalCell_Invisible)
    {
        Dest->GlyphIndex = 0;
    }

    Dest->Foreground = Foreground | (Props.Flags << 24);
    Dest->Background = Background;
}

static void ClearProps(example_terminal *Terminal, glyph_props *Props)
{
    Props->Foreground = Terminal->DefaultForegroundColor;
    Props->Background = Terminal->DefaultBackgroundColor;
    Props->Flags = 0;
}

static void ClearCursor(example_terminal *Terminal, cursor_state *Cursor)
{
    Cursor->At.X = 0;
    Cursor->At.Y = 0;
    ClearProps(Terminal, &Cursor->Props);
}

static int ParseEscape(example_terminal *Terminal, source_buffer_range *Range, cursor_state *Cursor)
{
    int MovedCursor = 0;

    GetToken(Range);
    GetToken(Range);

    wchar_t Command = 0;
    uint32_t ParamCount = 0;
    uint32_t Params[8];
    while((ParamCount < ArrayCount(Params)) && Range->Count)
    {
        char Token = PeekToken(Range, 0);
        if(IsDigit(Token))
        {
            Params[ParamCount++] = ParseNumber(Range);
            wchar_t Semi = GetToken(Range);
            if(Semi != ';')
            {
                Command = Semi;
                break;
            }
        }
        else
        {
            Command = GetToken(Range);
        }
    }

    switch(Command)
    {
        case 'H':
        {
            // NOTE(casey): Move cursor to X,Y position
            Cursor->At.X = Params[1] - 1;
            Cursor->At.Y = Params[0] - 1;
            MovedCursor = 1;
        } break;

        case 'm':
        {
            // NOTE(casey): Set graphics mode
            if(Params[0] == 0)
            {
                ClearProps(Terminal, &Cursor->Props);
            }

            if(Params[0] == 1) Cursor->Props.Flags |= TerminalCell_Bold;
            if(Params[0] == 2) Cursor->Props.Flags |= TerminalCell_Dim;
            if(Params[0] == 3) Cursor->Props.Flags |= TerminalCell_Italic;
            if(Params[0] == 4) Cursor->Props.Flags |= TerminalCell_Underline;
            if(Params[0] == 5) Cursor->Props.Flags |= TerminalCell_Blinking;
            if(Params[0] == 7) Cursor->Props.Flags |= TerminalCell_ReverseVideo;
            if(Params[0] == 8) Cursor->Props.Flags |= TerminalCell_Invisible;
            if(Params[0] == 9) Cursor->Props.Flags |= TerminalCell_Strikethrough;

            if((Params[0] == 38) && (Params[1] == 2)) Cursor->Props.Foreground = PackRGB(Params[2], Params[3], Params[4]);
            if((Params[0] == 48) && (Params[1] == 2)) Cursor->Props.Background = PackRGB(Params[2], Params[3], Params[4]);
        } break;
    }

    return MovedCursor;
}

static size_t GetLineLength(example_line *Line)
{
    Assert(Line->OnePastLastP >= Line->FirstP);
    size_t Result = Line->OnePastLastP - Line->FirstP;
    return Result;
}

static void ParseLines(example_terminal *Terminal, source_buffer_range Range, cursor_state *Cursor)
{
    /* TODO(casey): Currently, if the commit of line data _straddles_ a control code boundary
       this code does not properly _stop_ the processing cursor.  This can cause an edge case
       where a VT code that splits a line _doesn't_ split the line as it should.  To fix this
       the ending code just needs to check to see if the reason it couldn't parse an escape
       code in "AtEscape" was that it ran out of characters, and if so, don't advance the parser
       past that point.
    */

    __m128i Carriage = _mm_set1_epi8('\n');
    __m128i Escape = _mm_set1_epi8('\x1b');
    __m128i Complex = _mm_set1_epi8(0x80);

    size_t SplitLineAtCount = 4096;
    size_t LastP = Range.AbsoluteP;
    while(Range.Count)
    {
        __m128i ContainsComplex = _mm_setzero_si128();
        size_t Count = Range.Count;
        if(Count > SplitLineAtCount) Count = SplitLineAtCount;
        char *Data = Range.Data;
        while(Count >= 16)
        {
            __m128i Batch = _mm_loadu_si128((__m128i *)Data);
            __m128i TestC = _mm_cmpeq_epi8(Batch, Carriage);
            __m128i TestE = _mm_cmpeq_epi8(Batch, Escape);
            __m128i TestX = _mm_and_si128(Batch, Complex);
            __m128i Test = _mm_or_si128(TestC, TestE);
            int Check = _mm_movemask_epi8(Test);
            if(Check)
            {
                int Advance = _tzcnt_u32(Check);
                __m128i MaskX = _mm_loadu_si128((__m128i *)(OverhangMask + 16 - Advance));
                TestX = _mm_and_si128(MaskX, TestX);
                ContainsComplex = _mm_or_si128(ContainsComplex, TestX);
                Count -= Advance;
                Data += Advance;
                break;
            }

            ContainsComplex = _mm_or_si128(ContainsComplex, TestX);
            Count -= 16;
            Data += 16;
        }

        Range = ConsumeCount(Range, Data - Range.Data);

        Terminal->Lines[Terminal->CurrentLineIndex].ContainsComplexChars |=
            _mm_movemask_epi8(ContainsComplex);

        if(AtEscape(&Range))
        {
            size_t FeedAt = Range.AbsoluteP;
            if(ParseEscape(Terminal, &Range, Cursor))
            {
                LineFeed(Terminal, FeedAt, FeedAt, Cursor->Props);
            }
        }
        else
        {
            char Token = GetToken(&Range);
            if(Token == '\n')
            {
                LineFeed(Terminal, Range.AbsoluteP, Range.AbsoluteP, Cursor->Props);
            }
            else if(Token < 0) // TODO(casey): Not sure what is a "combining char" here, really, but this is a rough test
            {
                Terminal->Lines[Terminal->CurrentLineIndex].ContainsComplexChars = 1;
            }
        }

        UpdateLineEnd(Terminal, Range.AbsoluteP);
        if(GetLineLength(&Terminal->Lines[Terminal->CurrentLineIndex]) > SplitLineAtCount)
        {
            LineFeed(Terminal, Range.AbsoluteP, Range.AbsoluteP, Cursor->Props);
        }
    }
}

static void ParseWithUniscribe(example_terminal *Terminal, source_buffer_range UTF8Range, cursor_state *Cursor)
{
    /* TODO(casey): This code is absolutely horrible - Uniscribe is basically unusable as an API, because it doesn't support
       a clean state-machine way of feeding things to it.  So I don't even know how you would really use it in a way
       that guaranteed you didn't have buffer-too-small problems.  It's just horrible.

       Plus, it is UTF16, which means we have to do an entire conversion here before we even call it :(

       I would rather get rid of this function altogether and move to something that supports UTF8, because we never
       actually want to have to do this garbage - it's just a giant hack for no reason.  Basically everything in this
       function should be replaced by something that can turn UTF8 into chunks that need to be rasterized together.

       That's all we need here, and it could be done very efficiently with sensible code.
    */

    example_partitioner *Partitioner = &Terminal->Partitioner;

    DWORD Count = MultiByteToWideChar(CP_UTF8, 0, UTF8Range.Data, (DWORD)UTF8Range.Count,
                                      Partitioner->Expansion, ArrayCount(Partitioner->Expansion));
    wchar_t *Data = Partitioner->Expansion;

    int ItemCount = 0;
    ScriptItemize(Data, Count, ArrayCount(Partitioner->Items), &Partitioner->UniControl, &Partitioner->UniState, Partitioner->Items, &ItemCount);

    int Segment = 0;

    for(int ItemIndex = 0;
        ItemIndex < ItemCount;
        ++ItemIndex)
    {
        SCRIPT_ITEM *Item = Partitioner->Items + ItemIndex;

        Assert((DWORD)Item->iCharPos < Count);
        DWORD StrCount = Count - Item->iCharPos;
        if((ItemIndex + 1) < ItemCount)
        {
            Assert(Item[1].iCharPos >= Item[0].iCharPos);
            StrCount = (Item[1].iCharPos - Item[0].iCharPos);
        }

        wchar_t *Str = Data + Item->iCharPos;

        int IsComplex = (ScriptIsComplex(Str, StrCount, SIC_COMPLEX) == S_OK);
        ScriptBreak(Str, StrCount, &Item->a, Partitioner->Log);

        int SegCount = 0;

        Partitioner->SegP[SegCount++] = 0;
        for(uint32_t CheckIndex = 0;
            CheckIndex < StrCount;
            ++CheckIndex)
        {
            SCRIPT_LOGATTR Attr = Partitioner->Log[CheckIndex];
            int ShouldBreak = (Str[CheckIndex] == ' ');;
            if(IsComplex)
            {
                ShouldBreak |= Attr.fSoftBreak;
            }
            else
            {
                ShouldBreak |= Attr.fCharStop;
            }

            if(ShouldBreak) Partitioner->SegP[SegCount++] = CheckIndex;
        }
        Partitioner->SegP[SegCount++] = StrCount;

        int dSeg = 1;
        int SegStart = 0;
        int SegStop = SegCount - 1;
        if(Item->a.fRTL || Item->a.fLayoutRTL)
        {
            dSeg = -1;
            SegStart = SegCount - 2;
            SegStop = -1;
        }

        for(int SegIndex = SegStart;
            SegIndex != SegStop;
            SegIndex += dSeg)
        {
            size_t Start = Partitioner->SegP[SegIndex];
            size_t End = Partitioner->SegP[SegIndex + 1];
            size_t ThisCount = (End - Start);
            if(ThisCount)
            {
                wchar_t *Run = Str + Start;
                wchar_t CodePoint = Run[0];
                if((ThisCount == 1) && IsDirectCodepoint(CodePoint))
                {
                    renderer_cell *Cell = GetCell(&Terminal->ScreenBuffer, Cursor->At);
                    if(Cell)
                    {
                        glyph_props Props = Cursor->Props;
                        if(Terminal->DebugHighlighting)
                        {
                            Props.Background = 0x00800000;
                        }
                        SetCellDirect(Terminal->ReservedTileTable[CodePoint - MinDirectCodepoint], Props, Cell);
                    }

                    AdvanceColumn(Terminal, &Cursor->At);
                }
                else
                {
                    // TODO(casey): This wastes a lookup on the tile count.
                    // It should save the entry somehow, and roll it into the first cell.

                    int Prepped = 0;
                    glyph_hash RunHash = ComputeGlyphHash(2*ThisCount, (char unsigned *)Run, DefaultSeed);
                    glyph_dim GlyphDim = GetGlyphDim(&Terminal->GlyphGen, Terminal->GlyphTable, ThisCount, Run, RunHash);
                    for(uint32_t TileIndex = 0;
                        TileIndex < GlyphDim.TileCount;
                        ++TileIndex)
                    {
                        renderer_cell *Cell = GetCell(&Terminal->ScreenBuffer, Cursor->At);
                        if(Cell)
                        {
                            glyph_hash TileHash = ComputeHashForTileIndex(RunHash, TileIndex);
                            glyph_state Entry = FindGlyphEntryByHash(Terminal->GlyphTable, TileHash);
                            if(Entry.FilledState != GlyphState_Rasterized)
                            {
                                if(!Prepped)
                                {
                                    PrepareTilesForTransfer(&Terminal->GlyphGen, &Terminal->Renderer, ThisCount, Run, GlyphDim);
                                    Prepped = 1;
                                }

                                TransferTile(&Terminal->GlyphGen, &Terminal->Renderer, TileIndex, Entry.GPUIndex);
                                UpdateGlyphCacheEntry(Terminal->GlyphTable, Entry.ID, GlyphState_Rasterized, Entry.DimX, Entry.DimY);
                            }

                            glyph_props Props = Cursor->Props;
                            if(Terminal->DebugHighlighting)
                            {
                                Props.Background = Segment ? 0x0008080 : 0x00000080;
                                Segment = !Segment;
                            }
                            SetCellDirect(Entry.GPUIndex, Props, Cell);
                        }

                        AdvanceColumn(Terminal, &Cursor->At);
                    }
                }
            }
        }
    }
}

static int ParseLineIntoGlyphs(example_terminal *Terminal, source_buffer_range Range,
                                cursor_state *Cursor, int ContainsComplexChars)
{
    int CursorJumped = 0;

    while(Range.Count)
    {
        // NOTE(casey): Eat all non-Unicode
        char Peek = PeekToken(&Range, 0);
        if((Peek == '\x1b') && AtEscape(&Range))
        {
            if(ParseEscape(Terminal, &Range, Cursor))
            {
                CursorJumped = 1;
            }
        }
        else if(Peek == '\r')
        {
            GetToken(&Range);
            Cursor->At.X = 0;
        }
        else if(Peek == '\n')
        {
            GetToken(&Range);
            AdvanceRow(Terminal, &Cursor->At);
        }
        else if(ContainsComplexChars)
        {
            /* TODO(casey): Currently, if you have a long line that force-splits, it will not
               recombine properly with Unicode.  I _DO NOT_ think this should be fixed in
               the line parser.  Instead, the fix should be what should happen here to begin
               with, which is that the glyph chunking should happen in a state machine,
               NOT using buffer runs like Uniscribe does.

               So I believe the _correct_ design here is that you have a state machine instead
               of Uniscribe for complex grapheme clusters, and _that_ will "just work" here
               as well as being much much faster than the current path, which is very slow
               because of Uniscribe _and_ is limited to intermediate buffer sizes.
            */

            // NOTE(casey): If it's not an escape, and this line contains fancy Unicode stuff,
            // it's something we need to pass to a shaper to find out how it
            // has to be segmented.  Which sadly is Uniscribe at this point :(
            // Putting something actually good in here would probably be a massive improvement.

            // NOTE(casey): Scan for the next escape code (which Uniscribe helpfully totally fails to handle)
            source_buffer_range SubRange = Range;
            do
            {
                Range = ConsumeCount(Range, 1);
            } while(Range.Count &&
                        (Range.Data[0] != '\n') &&
                        (Range.Data[0] != '\r') &&
                        (Range.Data[0] != '\x1b'));


            // NOTE(casey): Pass the range between the escape codes to Uniscribe
            SubRange.Count = Range.AbsoluteP - SubRange.AbsoluteP;
            ParseWithUniscribe(Terminal, SubRange, Cursor);
        }
        else
        {
            // NOTE(casey): It's not an escape, and we know there are only simple characters on the line.

            wchar_t CodePoint = GetToken(&Range);
            renderer_cell *Cell = GetCell(&Terminal->ScreenBuffer, Cursor->At);
            if(Cell)
            {
                gpu_glyph_index GPUIndex = {0};
                if(IsDirectCodepoint(CodePoint))
                {
                    GPUIndex = Terminal->ReservedTileTable[CodePoint - MinDirectCodepoint];
                }
                else
                {
                    Assert(CodePoint <= 127);
                    glyph_hash RunHash = ComputeGlyphHash(2, (char unsigned *)&CodePoint, DefaultSeed);
                    glyph_state Entry = FindGlyphEntryByHash(Terminal->GlyphTable, RunHash);
                    if(Entry.FilledState != GlyphState_Rasterized)
                    {
                        PrepareTilesForTransfer(&Terminal->GlyphGen, &Terminal->Renderer, 1, &CodePoint, GetSingleTileUnitDim());
                        TransferTile(&Terminal->GlyphGen, &Terminal->Renderer, 0, Entry.GPUIndex);
                        UpdateGlyphCacheEntry(Terminal->GlyphTable, Entry.ID, GlyphState_Rasterized, Entry.DimX, Entry.DimY);
                    }
                    GPUIndex = Entry.GPUIndex;
                }

                SetCellDirect(GPUIndex, Cursor->Props, Cell);
            }

            AdvanceColumn(Terminal, &Cursor->At);
        }
    }

    return CursorJumped;
}

static void CloseProcess(example_terminal *Terminal)
{
    CloseHandle(Terminal->ChildProcess);
    CloseHandle(Terminal->Legacy_WriteStdIn);
    CloseHandle(Terminal->Legacy_ReadStdOut);
    CloseHandle(Terminal->Legacy_ReadStdError);
    CloseHandle(Terminal->FastPipe);

    Terminal->ChildProcess = INVALID_HANDLE_VALUE;
    Terminal->Legacy_WriteStdIn = INVALID_HANDLE_VALUE;
    Terminal->Legacy_ReadStdOut = INVALID_HANDLE_VALUE;
    Terminal->Legacy_ReadStdError = INVALID_HANDLE_VALUE;
    Terminal->FastPipe = INVALID_HANDLE_VALUE;
}

static void KillProcess(example_terminal *Terminal)
{
    if(Terminal->ChildProcess != INVALID_HANDLE_VALUE)
    {
        TerminateProcess(Terminal->ChildProcess, 0);
        CloseProcess(Terminal);
    }
}

static void AppendOutput(example_terminal *Terminal, char *Format, ...)
{
    // TODO(casey): This is all garbage code.  You need a checked printf here, and of
    // course there isn't one of those.  Ideally this would change over to using
    // a real concatenator here, like with a #define system, but this is just
    // a hack for now to do basic printing from the internal code.

    source_buffer_range Dest = GetNextWritableRange(&Terminal->ScrollBackBuffer, LARGEST_AVAILABLE);
    va_list ArgList;
    va_start(ArgList, Format);
    int Used = wvsprintfA(Dest.Data, Format, ArgList);
    va_end(ArgList);

    Dest.Count = Used;
    CommitWrite(&Terminal->ScrollBackBuffer, Dest.Count);
    ParseLines(Terminal, Dest, &Terminal->RunningCursor);
}

static int UpdateTerminalBuffer(example_terminal *Terminal, HANDLE FromPipe)
{
    int Result = 0;

    if(FromPipe != INVALID_HANDLE_VALUE)
    {
        Result = 1;

        terminal_buffer *Term = &Terminal->ScreenBuffer;

        DWORD PendingCount = GetPipePendingDataCount(FromPipe);
        if(PendingCount)
        {
            source_buffer_range Dest = GetNextWritableRange(&Terminal->ScrollBackBuffer, PendingCount);

            DWORD ReadCount = 0;
            if(ReadFile(FromPipe, Dest.Data, (DWORD)Dest.Count, &ReadCount, 0))
            {
                Assert(ReadCount <= Dest.Count);
                Dest.Count = ReadCount;
                CommitWrite(&Terminal->ScrollBackBuffer, Dest.Count);
                ParseLines(Terminal, Dest, &Terminal->RunningCursor);
            }
        }
        else
        {
            DWORD Error = GetLastError();
            if((Error == ERROR_BROKEN_PIPE) ||
               (Error == ERROR_INVALID_HANDLE))
            {
                Result = 0;
            }
        }
    }

    return Result;
}

static void LayoutLines(example_terminal *Terminal)
{
    // TODO(casey): Probably want to do something better here - this over-clears, since we clear
    // the whole thing and then also each line, for no real reason other than to make line wrapping
    // simpler.
    Clear(Terminal, &Terminal->ScreenBuffer);

    //
    // TODO(casey): This code is super bad, and there's no need for it to keep repeating itself.
    //

    // TODO(casey): How do we know how far back to go, for control chars?
    int32_t LineCount = 2*Terminal->ScreenBuffer.DimY;
    int32_t LineOffset = Terminal->CurrentLineIndex + Terminal->ViewingLineOffset - LineCount;

    int CursorJumped = 0;

    cursor_state Cursor = {0};
    ClearCursor(Terminal, &Cursor);
    for(int32_t LineIndexIndex = 0;
        LineIndexIndex < LineCount;
        ++LineIndexIndex)
    {
        int32_t LineIndex = (LineOffset + LineIndexIndex) % Terminal->MaxLineCount;
        if(LineIndex < 0) LineIndex += Terminal->MaxLineCount;

        example_line Line = Terminal->Lines[LineIndex];

        source_buffer_range Range = ReadSourceAt(&Terminal->ScrollBackBuffer, Line.FirstP, Line.OnePastLastP - Line.FirstP);
        Cursor.Props = Line.StartingProps;
        if(ParseLineIntoGlyphs(Terminal, Range, &Cursor, Line.ContainsComplexChars))
        {
            CursorJumped = 1;
        }
    }

    if(CursorJumped)
    {
        Cursor.At.X = 0;
        Cursor.At.Y = Terminal->ScreenBuffer.DimY - 4;
    }

    AdvanceRow(Terminal, &Cursor.At);
    ClearProps(Terminal, &Cursor.Props);

#if 0
    uint32_t CLCount = Terminal->CommandLineCount;

    source_buffer_range CommandLineRange = {0};
    CommandLineRange.AbsoluteP = 0;
    CommandLineRange.Count = CLCount;
    CommandLineRange.Data = (char *)Terminal->CommandLine;
#else
#endif
    char Prompt[] = {'>', ' '};
    source_buffer_range PromptRange = {0};
    PromptRange.Count = ArrayCount(Prompt);
    PromptRange.Data = Prompt;
    ParseLineIntoGlyphs(Terminal, PromptRange, &Cursor, 0);

    source_buffer_range CommandLineRange = {0};
    CommandLineRange.Count = Terminal->CommandLineCount;
    CommandLineRange.Data = Terminal->CommandLine;
    ParseLineIntoGlyphs(Terminal, CommandLineRange, &Cursor, 1);

    char CursorCode[] = {'\x1b', '[', '5',  'm', 0xe2, 0x96, 0x88};
    source_buffer_range CursorRange = {0};
    CursorRange.Count = ArrayCount(CursorCode);
    CursorRange.Data = CursorCode;
    ParseLineIntoGlyphs(Terminal, CursorRange, &Cursor, 1);
    AdvanceRowNoClear(Terminal, &Cursor.At);

    Terminal->ScreenBuffer.FirstLineY = CursorJumped ? 0 : Cursor.At.Y;
}

static int ExecuteSubProcess(example_terminal *Terminal, char *ProcessName, char *ProcessCommandLine)
{
    if(Terminal->ChildProcess != INVALID_HANDLE_VALUE)
    {
        KillProcess(Terminal);
    }

    PROCESS_INFORMATION ProcessInfo = {0};
    STARTUPINFOA StartupInfo = {sizeof(StartupInfo)};
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;

    SECURITY_ATTRIBUTES Inherit = {sizeof(Inherit)};
    Inherit.bInheritHandle = TRUE;

    CreatePipe(&StartupInfo.hStdInput, &Terminal->Legacy_WriteStdIn, &Inherit, 0);
    CreatePipe(&Terminal->Legacy_ReadStdOut, &StartupInfo.hStdOutput, &Inherit, 0);
    CreatePipe(&Terminal->Legacy_ReadStdError, &StartupInfo.hStdError, &Inherit, 0);

    SetHandleInformation(Terminal->Legacy_WriteStdIn, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(Terminal->Legacy_ReadStdOut, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(Terminal->Legacy_ReadStdError, HANDLE_FLAG_INHERIT, 0);

    int Result = 0;

    char *ProcessDir = ".\\";
    if(CreateProcessA(
        ProcessName,
        ProcessCommandLine,
        0,
        0,
        TRUE,
        CREATE_NO_WINDOW|CREATE_SUSPENDED,
        0,
        ProcessDir,
        &StartupInfo,
        &ProcessInfo))
    {
        if(Terminal->EnableFastPipe)
        {
            wchar_t PipeName[64];
            wsprintfW(PipeName, L"\\\\.\\pipe\\fastpipe%x", ProcessInfo.dwProcessId);
            Terminal->FastPipe = CreateNamedPipeW(PipeName, PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED, 0, 1,
                                                      Terminal->PipeSize, Terminal->PipeSize, 0, 0);

            // TODO(casey): Should give this its own event / overlapped
            ConnectNamedPipe(Terminal->FastPipe, &Terminal->FastPipeTrigger);

            DWORD Error = GetLastError();
            Assert(Error == ERROR_IO_PENDING);
        }

        ResumeThread(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hThread);
        Terminal->ChildProcess = ProcessInfo.hProcess;

        Result = 1;
    }

    CloseHandle(StartupInfo.hStdInput);
    CloseHandle(StartupInfo.hStdOutput);
    CloseHandle(StartupInfo.hStdError);

    return Result;
}

static int StringsAreEqual(char *A, char *B)
{
    if(A && B)
    {
        while(*A && *B && (*A == *B))
        {
            ++A;
            ++B;
        }

        return(*A == *B);
    }
    else
    {
        return(A == B);
    }
}

static void
RevertToDefaultFont(example_terminal *Terminal)
{
#if 0
    wsprintfW(Terminal->RequestedFontName, L"%s", L"Courier New");
    Terminal->RequestedFontHeight = 25;
#else
    wsprintfW(Terminal->RequestedFontName, L"%s", L"Cascadia Mono");
    Terminal->RequestedFontHeight = 17;
#endif
}

static int
RefreshFont(example_terminal *Terminal)
{
    int Result = 0;

    //
    // NOTE(casey): Set up the mapping table between run-hashes and glyphs
    //

    glyph_table_params Params = {0};

    // NOTE(casey): An additional tile is reserved for position 0, so it can be "empty",
    // in case the space glyph is not actually empty.
    Params.ReservedTileCount = ArrayCount(Terminal->ReservedTileTable) + 1;

    // NOTE(casey): We have to shrink the font size until it fits in the glyph texture,
    // to prevent large fonts from overflowing.
    for(int Try = 0; Try <= 1; ++Try)
    {
        Result = SetFont(&Terminal->GlyphGen, Terminal->RequestedFontName, Terminal->RequestedFontHeight);
        if(Result)
        {
            Params.CacheTileCountInX = SafeRatio1(Terminal->REFTERM_TEXTURE_WIDTH, Terminal->GlyphGen.FontWidth);
            Params.EntryCount = GetExpectedTileCountForDimension(&Terminal->GlyphGen, Terminal->REFTERM_TEXTURE_WIDTH, Terminal->REFTERM_TEXTURE_HEIGHT);
            Params.HashCount = 4096;

            if(Params.EntryCount > Params.ReservedTileCount)
            {
                Params.EntryCount -= Params.ReservedTileCount;
                break;
            }
        }

        RevertToDefaultFont(Terminal);
    }

    // TODO(casey): In theory, this VirtualAlloc could fail, so it may be a better idea to
    // just use a reserved memory footprint here and always use the same size.  It is not
    // a very large amount of memory, so picking a maximum and sticking with it is probably
    // better.  You can cap the size of the cache and there is no real penalty for doing
    // that, since it's a cache, so it'd just be a better idea all around.
    if(Terminal->GlyphTableMem)
    {
        VirtualFree(Terminal->GlyphTableMem, 0, MEM_RELEASE);
        Terminal->GlyphTableMem = 0;
    }
    Terminal->GlyphTableMem = VirtualAlloc(0, GetGlyphTableFootprint(Params), MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    Terminal->GlyphTable = PlaceGlyphTableInMemory(Params, Terminal->GlyphTableMem);

    InitializeDirectGlyphTable(Params, Terminal->ReservedTileTable, 1);

    //
    // NOTE(casey): Pre-rasterize all the ASCII characters, since they are directly mapped rather than hash-mapped.
    //

    glyph_dim UnitDim = GetSingleTileUnitDim();

    for(uint32_t TileIndex = 0;
        TileIndex < ArrayCount(Terminal->ReservedTileTable);
        ++TileIndex)
    {
        wchar_t Letter = MinDirectCodepoint + TileIndex;
        PrepareTilesForTransfer(&Terminal->GlyphGen, &Terminal->Renderer, 1, &Letter, UnitDim);
        TransferTile(&Terminal->GlyphGen, &Terminal->Renderer, 0, Terminal->ReservedTileTable[TileIndex]);
    }

    // NOTE(casey): Clear the reserved 0 tile
    wchar_t Nothing = 0;
    gpu_glyph_index ZeroTile = {0};
    PrepareTilesForTransfer(&Terminal->GlyphGen, &Terminal->Renderer, 0, &Nothing, UnitDim);
    TransferTile(&Terminal->GlyphGen, &Terminal->Renderer, 0, ZeroTile);

    return Result;
}

static void ExecuteCommandLine(example_terminal *Terminal)
{
    // TODO(casey): All of this is complete garbage and should never ever be used.

    Terminal->CommandLine[Terminal->CommandLineCount] = 0;
    uint32_t ParamStart = 0;
    while(ParamStart < Terminal->CommandLineCount)
    {
        if(Terminal->CommandLine[ParamStart] == ' ') break;
        ++ParamStart;
    }

    char *A = Terminal->CommandLine;
    char *B = Terminal->CommandLine + ParamStart;
    *B = 0;
    if(ParamStart < Terminal->CommandLineCount)
    {
        ++ParamStart;
        ++B;
    }

    source_buffer_range ParamRange = {0};
    ParamRange.Data = B;
    ParamRange.Count = Terminal->CommandLineCount - ParamStart;

    // TODO(casey): Collapse all these options into a little array, so there's no
    // copy-pasta.
    AppendOutput(Terminal, "\n");
    if(StringsAreEqual(Terminal->CommandLine, "status"))
    {
        ClearProps(Terminal, &Terminal->RunningCursor.Props);
        AppendOutput(Terminal, "RefTerm v%u\n", REFTERM_VERSION);
        AppendOutput(Terminal, "Size: %u x %u\n", Terminal->ScreenBuffer.DimX, Terminal->ScreenBuffer.DimY);
        AppendOutput(Terminal, "Fast pipe: %s\n", Terminal->EnableFastPipe ? "ON" : "off");
        AppendOutput(Terminal, "Font: %S %u\n", Terminal->RequestedFontName, Terminal->RequestedFontHeight);
        AppendOutput(Terminal, "Line Wrap: %s\n", Terminal->LineWrap ? "ON" : "off");
        AppendOutput(Terminal, "Debug: %s\n", Terminal->DebugHighlighting ? "ON" : "off");
        AppendOutput(Terminal, "Throttling: %s\n", !Terminal->NoThrottle ? "ON" : "off");
    }
    else if(StringsAreEqual(Terminal->CommandLine, "fastpipe"))
    {
        Terminal->EnableFastPipe = !Terminal->EnableFastPipe;
        AppendOutput(Terminal, "Fast pipe: %s\n", Terminal->EnableFastPipe ? "ON" : "off");
    }
    else if(StringsAreEqual(Terminal->CommandLine, "linewrap"))
    {
        Terminal->LineWrap = !Terminal->LineWrap;
        AppendOutput(Terminal, "LineWrap: %s\n", Terminal->LineWrap ? "ON" : "off");
    }
    else if(StringsAreEqual(Terminal->CommandLine, "debug"))
    {
        Terminal->DebugHighlighting = !Terminal->DebugHighlighting;
        AppendOutput(Terminal, "Debug: %s\n", Terminal->DebugHighlighting ? "ON" : "off");
    }
    else if(StringsAreEqual(Terminal->CommandLine, "throttle"))
    {
        Terminal->NoThrottle = !Terminal->NoThrottle;
        AppendOutput(Terminal, "Throttling: %s\n", !Terminal->NoThrottle ? "ON" : "off");
    }
    else if(StringsAreEqual(Terminal->CommandLine, "font"))
    {
        DWORD NullAt = MultiByteToWideChar(CP_UTF8, 0, B, (DWORD)(Terminal->CommandLineCount - ParamStart),
                                           Terminal->RequestedFontName, ArrayCount(Terminal->RequestedFontName) - 1);
        Terminal->RequestedFontName[NullAt] = 0;

        RefreshFont(Terminal);
        AppendOutput(Terminal, "Font: %S\n", Terminal->RequestedFontName);
    }
    else if(StringsAreEqual(Terminal->CommandLine, "fontsize"))
    {
        Terminal->RequestedFontHeight = ParseNumber(&ParamRange);
        RefreshFont(Terminal);
        AppendOutput(Terminal, "Font height: %u\n", Terminal->RequestedFontHeight);
    }
    else if((StringsAreEqual(Terminal->CommandLine, "kill")) ||
            (StringsAreEqual(Terminal->CommandLine, "break")))
    {
        KillProcess(Terminal);
    }
    else if((StringsAreEqual(Terminal->CommandLine, "clear")) ||
            (StringsAreEqual(Terminal->CommandLine, "cls")))
    {
        ClearCursor(Terminal, &Terminal->RunningCursor);
        memset(Terminal->Lines, 0, Terminal->MaxLineCount*sizeof(example_line));
    }
    else if((StringsAreEqual(Terminal->CommandLine, "exit")) ||
            (StringsAreEqual(Terminal->CommandLine, "quit")))
    {
        KillProcess(Terminal);
        AppendOutput(Terminal, "Exiting...\n");
        Terminal->Quit = 1;
    }
    else if((StringsAreEqual(Terminal->CommandLine, "echo")) ||
            (StringsAreEqual(Terminal->CommandLine, "print")))
    {
        AppendOutput(Terminal, "%s\n", B);
    }
    else if(StringsAreEqual(Terminal->CommandLine, ""))
    {
    }
    else
    {
        char ProcessName[ArrayCount(Terminal->CommandLine) + 1];
        char ProcessCommandLine[ArrayCount(Terminal->CommandLine) + 1];
        wsprintfA(ProcessName, "%s.exe", A);
        wsprintfA(ProcessCommandLine, "%s.exe %s", A, B);
        if(!ExecuteSubProcess(Terminal, ProcessName, ProcessCommandLine))
        {
            wsprintfA(ProcessName, "c:\\Windows\\System32\\cmd.exe");
            wsprintfA(ProcessCommandLine, "cmd.exe /c %s.exe %s", A, B);
            if(!ExecuteSubProcess(Terminal, ProcessName, ProcessCommandLine))
            {
                AppendOutput(Terminal, "ERROR: Unable to execute %s\n", Terminal->CommandLine);
            }
        }
    }
}

static int IsUTF8Extension(char A)
{
    int Result = ((A & 0xc0) == 0x80);
    return Result;
}

static void ProcessMessages(example_terminal *Terminal)
{
    MSG Message;
    while(PeekMessageW(&Message, 0, 0, 0, PM_REMOVE))
    {
        switch(Message.message)
        {
            case WM_QUIT:
            {
                Terminal->Quit = 1;
            } break;

            case WM_KEYDOWN:
            {
                switch(Message.wParam)
                {
                    case VK_PRIOR:
                    {
                        Terminal->ViewingLineOffset -= Terminal->ScreenBuffer.DimY/2;
                    } break;

                    case VK_NEXT:
                    {
                        Terminal->ViewingLineOffset += Terminal->ScreenBuffer.DimY/2;
                    } break;
                }

                if(Terminal->ViewingLineOffset > 0)
                {
                    Terminal->ViewingLineOffset = 0;
                }

                if(Terminal->ViewingLineOffset < -(int)Terminal->LineCount)
                {
                    Terminal->ViewingLineOffset = -(int)Terminal->LineCount;
                }
            } break;

            case WM_CHAR:
            {
                switch(Message.wParam)
                {
                    case VK_BACK:
                    {
                        while((Terminal->CommandLineCount > 0) &&
                              IsUTF8Extension(Terminal->CommandLine[Terminal->CommandLineCount - 1]))
                        {
                            --Terminal->CommandLineCount;
                        }

                        if(Terminal->CommandLineCount > 0)
                        {
                            --Terminal->CommandLineCount;
                        }
                    } break;

                    case VK_RETURN:
                    {
                        ExecuteCommandLine(Terminal);
                        Terminal->CommandLineCount = 0;
                        Terminal->ViewingLineOffset = 0;
                    } break;

                    default:
                    {
                        wchar_t Char = (wchar_t)Message.wParam;
                        wchar_t Chars[2];
                        int CharCount = 0;

                        if(IS_HIGH_SURROGATE(Char))
                        {
                            Terminal->LastChar = Char;
                        }
                        else if(IS_LOW_SURROGATE(Char))
                        {
                            if(IS_SURROGATE_PAIR(Terminal->LastChar, Char))
                            {
                                Chars[0] = Terminal->LastChar;
                                Chars[1] = Char;
                                CharCount = 2;
                            }
                            Terminal->LastChar = 0;
                        }
                        else
                        {
                            Chars[0] = Char;
                            CharCount = 1;
                        }

                        if(CharCount)
                        {
                            DWORD SpaceLeft = ArrayCount(Terminal->CommandLine) - Terminal->CommandLineCount;
                            Terminal->CommandLineCount +=
                                WideCharToMultiByte(CP_UTF8, 0,
                                                    Chars, CharCount,
                                                    Terminal->CommandLine + Terminal->CommandLineCount,
                                                    SpaceLeft, 0, 0);
                        }
                    } break;
                }
            } break;
        }
    }
}

static char OpeningMessage[] = { 0xE0, 0xA4, 0x9C, 0xE0, 0xA5, 0x8B, 0x20, 0xE0, 0xA4, 0xB8, 0xE0, 0xA5, 0x8B, 0x20, 0xE0, 0xA4, 0xB0, 0xE0, 0xA4, 0xB9, 0xE0, 0xA4, 0xBE, 0x20, 0xE0, 0xA4, 0xB9, 0xE0, 0xA5, 0x8B, 0x20, 0xE0, 0xA4, 0x89, 0xE0, 0xA4, 0xB8, 0xE0, 0xA5, 0x87, 0x20, 0xE0, 0xA4, 0xA4, 0xE0, 0xA5, 0x8B, 0x20, 0xE0, 0xA4, 0x9C, 0xE0, 0xA4, 0x97, 0xE0, 0xA4, 0xBE, 0x20, 0xE0, 0xA4, 0xB8, 0xE0, 0xA4, 0x95, 0xE0, 0xA4, 0xA4, 0xE0, 0xA5, 0x87, 0x20, 0xE0, 0xA4, 0xB9, 0xE0, 0xA5, 0x88, 0xE0, 0xA4, 0x82, 0x2C, 0x20, 0xE0, 0xA4, 0xB2, 0xE0, 0xA5, 0x87, 0xE0, 0xA4, 0x95, 0xE0, 0xA4, 0xBF, 0xE0, 0xA4, 0xA8, 0x20, 0xE0, 0xA4, 0x9C, 0xE0, 0xA5, 0x8B, 0x20, 0xE0, 0xA4, 0x86, 0xE0, 0xA4, 0x81, 0xE0, 0xA4, 0x96, 0xE0, 0xA5, 0x87, 0x20, 0xE0, 0xA4, 0xAE, 0xE0, 0xA5, 0x82, 0xE0, 0xA4, 0x81, 0xE0, 0xA4, 0xA6, 0x20, 0xE0, 0xA4, 0x95, 0xE0, 0xA4, 0xB0, 0x20, 0xE0, 0xA4, 0xB8, 0xE0, 0xA5, 0x8B, 0xE0, 0xA4, 0xA8, 0xE0, 0xA5, 0x87, 0x20, 0xE0, 0xA4, 0x95, 0xE0, 0xA4, 0xBE, 0x20, 0xE0, 0xA4, 0x85, 0xE0, 0xA4, 0xAD, 0xE0, 0xA4, 0xBF, 0xE0, 0xA4, 0xA8, 0xE0, 0xA4, 0xAF, 0x20, 0xE0, 0xA4, 0x95, 0xE0, 0xA4, 0xB0, 0x20, 0xE0, 0xA4, 0xB0, 0xE0, 0xA4, 0xB9, 0xE0, 0xA4, 0xBE, 0x20, 0xE0, 0xA4, 0xB9, 0xE0, 0xA5, 0x8B, 0x20, 0xE0, 0xA4, 0x89, 0xE0, 0xA4, 0xB8, 0xE0, 0xA5, 0x87, 0x20, 0xE0, 0xA4, 0x95, 0xE0, 0xA5, 0x88, 0xE0, 0xA4, 0xB8, 0xE0, 0xA5, 0x87, 0x20, 0xE0, 0xA4, 0x9C, 0xE0, 0xA4, 0x97, 0xE0, 0xA4, 0xBE, 0xE0, 0xA4, 0x8F, 0xE0, 0xA4, 0x82, 0xE0, 0xA4, 0x97, 0xE0, 0xA5, 0x87, 0x20, 0x7C, 0x20, '\n' };
static DWORD WINAPI TerminalThread(LPVOID Param)
{
    example_terminal *Terminal = VirtualAlloc(0, sizeof(example_terminal), MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    Terminal->Window = (HWND)Param;
    Terminal->LineWrap = 1;
    Terminal->ChildProcess = INVALID_HANDLE_VALUE;
    Terminal->Legacy_WriteStdIn = INVALID_HANDLE_VALUE;
    Terminal->Legacy_ReadStdOut = INVALID_HANDLE_VALUE;
    Terminal->Legacy_ReadStdError = INVALID_HANDLE_VALUE;
    Terminal->FastPipe = INVALID_HANDLE_VALUE;
    Terminal->DefaultForegroundColor = 0x00afafaf;
    Terminal->DefaultBackgroundColor = 0x000c0c0c;
    Terminal->FastPipeReady = CreateEventW(0, TRUE, FALSE, 0);
    Terminal->FastPipeTrigger.hEvent = Terminal->FastPipeReady;
    Terminal->PipeSize = 16*1024*1024;

    ClearCursor(Terminal, &Terminal->RunningCursor);

    // TODO(casey): I believe this should probably be sized to be the same
    // as the window at a minimum, because if it isn't, you may run into
    // pathological cases where the wrong glyph is rendered.  The alternative
    // would be forcing flushes of the pipe when the total number of recycles
    // in a frame reaches the total number of glyphs in the cache.  Since the
    // basically never happens, you don't see any bugs with it, but it
    // theoretically _could_ happen if the texture size isn't large enough
    // to fit one whole screen of glyphs.
    Terminal->REFTERM_TEXTURE_WIDTH = 2048;
    Terminal->REFTERM_TEXTURE_HEIGHT = 2048;

    // TODO(casey): Auto-size this, somehow?  The TransferHeight effectively restricts the maximum size of the
    // font, so it may want to be "grown" based on the font size selected.
    Terminal->TransferWidth = 1024;
    Terminal->TransferHeight = 512;

    Terminal->REFTERM_MAX_WIDTH = 1024;
    Terminal->REFTERM_MAX_HEIGHT = 1024;

    int DebugD3D11 = 0;
#if _DEBUG
    DebugD3D11 = 1;
#endif

    Terminal->Renderer = AcquireD3D11Renderer(Terminal->Window, DebugD3D11);
    SetD3D11GlyphCacheDim(&Terminal->Renderer, Terminal->REFTERM_TEXTURE_WIDTH, Terminal->REFTERM_TEXTURE_HEIGHT);
    SetD3D11GlyphTransferDim(&Terminal->Renderer, Terminal->TransferWidth, Terminal->TransferHeight);

    Terminal->GlyphGen = AllocateGlyphGenerator(Terminal->TransferWidth, Terminal->TransferHeight, Terminal->Renderer.GlyphTransferSurface);
    Terminal->ScrollBackBuffer = AllocateSourceBuffer(Terminal->PipeSize);

    ScriptRecordDigitSubstitution(LOCALE_USER_DEFAULT, &Terminal->Partitioner.UniDigiSub); // TODO(casey): Move this out to the stored code
    ScriptApplyDigitSubstitution(&Terminal->Partitioner.UniDigiSub, &Terminal->Partitioner.UniControl, &Terminal->Partitioner.UniState);

    Terminal->MaxLineCount = 8192;
    Terminal->Lines = VirtualAlloc(0, Terminal->MaxLineCount*sizeof(example_line), MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);

    RevertToDefaultFont(Terminal);
    RefreshFont(Terminal);

    ShowWindow(Terminal->Window, SW_SHOWDEFAULT);

    AppendOutput(Terminal, "\n"); // TODO(casey): Better line startup - this is here just to initialize the running cursor.
    AppendOutput(Terminal, "Refterm v%u\n", REFTERM_VERSION);
    AppendOutput(Terminal,
                     "THIS IS \x1b[38;2;255;0;0m\x1b[5mNOT\x1b[0m A REAL \x1b[9mTERMINAL\x1b[0m.\r\n"
                     "It is a reference renderer for demonstrating how to easily build relatively efficient terminal displays.\r\n"
                     "\x1b[38;2;255;0;0m\x1b[5m\x1b[4mDO NOT\x1b[0m attempt to use this as your terminal, or you will be \x1b[2mvery\x1b[0m sad.\r\n"
                 );

    AppendOutput(Terminal, "\n");
    AppendOutput(Terminal, OpeningMessage);
    AppendOutput(Terminal, "\n");
    
    int BlinkMS = 500; // TODO(casey): Use this in blink determination
    int MinTermSize = 512;
    uint32_t Width = MinTermSize;
    uint32_t Height = MinTermSize;

    LARGE_INTEGER Frequency, Time;
    QueryPerformanceFrequency(&Frequency);
    QueryPerformanceCounter(&Time);

    LARGE_INTEGER StartTime;
    QueryPerformanceCounter(&StartTime);

    size_t FrameCount = 0;
    size_t FrameIndex = 0;
    int64_t UpdateTitle = Time.QuadPart + Frequency.QuadPart;

    wchar_t LastChar = 0;

    while(!Terminal->Quit)
    {
        if(!Terminal->NoThrottle)
        {
            HANDLE Handles[8];
            DWORD HandleCount = 0;

            Handles[HandleCount++] = Terminal->FastPipeReady;
            if(Terminal->Legacy_ReadStdOut != INVALID_HANDLE_VALUE) Handles[HandleCount++] = Terminal->Legacy_ReadStdOut;
            if(Terminal->Legacy_ReadStdError != INVALID_HANDLE_VALUE) Handles[HandleCount++] = Terminal->Legacy_ReadStdError;
            MsgWaitForMultipleObjects(HandleCount, Handles, FALSE, BlinkMS, QS_ALLINPUT);
        }

        ProcessMessages(Terminal);

        RECT Rect;
        GetClientRect(Terminal->Window, &Rect);

        if(((Rect.left + MinTermSize) <= Rect.right) &&
           ((Rect.top + MinTermSize) <= Rect.bottom))
        {
            Width = Rect.right - Rect.left;
            Height = Rect.bottom - Rect.top;

            uint32_t Margin = 8;
            uint32_t NewDimX = SafeRatio1(Width - Margin, Terminal->GlyphGen.FontWidth);
            uint32_t NewDimY = SafeRatio1(Height - Margin, Terminal->GlyphGen.FontHeight);
            if(NewDimX > Terminal->REFTERM_MAX_WIDTH) NewDimX = Terminal->REFTERM_MAX_WIDTH;
            if(NewDimY > Terminal->REFTERM_MAX_HEIGHT) NewDimY = Terminal->REFTERM_MAX_HEIGHT;

            // TODO(casey): Maybe only allocate on size differences,
            // etc. Make a real resize function here for people who care.
            if((Terminal->ScreenBuffer.DimX != NewDimX) ||
               (Terminal->ScreenBuffer.DimY != NewDimY))
            {
                DeallocateTerminalBuffer(&Terminal->ScreenBuffer);
                Terminal->ScreenBuffer = AllocateTerminalBuffer(NewDimX, NewDimY);
            }
        }

        do
        {
            int FastIn = UpdateTerminalBuffer(Terminal, Terminal->FastPipe);
            int SlowIn = UpdateTerminalBuffer(Terminal, Terminal->Legacy_ReadStdOut);
            int ErrIn = UpdateTerminalBuffer(Terminal, Terminal->Legacy_ReadStdError);

            if(!SlowIn && (Terminal->Legacy_ReadStdOut != INVALID_HANDLE_VALUE))
            {
                CloseHandle(Terminal->Legacy_ReadStdOut); // TODO(casey): Not sure if this is supposed to be called?
                Terminal->Legacy_ReadStdOut = INVALID_HANDLE_VALUE;
            }

            if(!ErrIn && (Terminal->Legacy_ReadStdError != INVALID_HANDLE_VALUE))
            {
                CloseHandle(Terminal->Legacy_ReadStdError); // TODO(casey): Not sure if this is supposed to be called?
                Terminal->Legacy_ReadStdError = INVALID_HANDLE_VALUE;
            }
        }
        while((Terminal->Renderer.FrameLatencyWaitableObject != INVALID_HANDLE_VALUE) &&
                  (WaitForSingleObject(Terminal->Renderer.FrameLatencyWaitableObject, 0) == WAIT_TIMEOUT));

        ResetEvent(Terminal->FastPipeReady);
        ReadFile(Terminal->FastPipe, 0, 0, 0, &Terminal->FastPipeTrigger);

        LayoutLines(Terminal);

        // TODO(casey): Split RendererDraw into two!
        // Update, and render, since we only need to update if we actually get new input.

        LARGE_INTEGER BlinkTimer;
        QueryPerformanceCounter(&BlinkTimer);
        int Blink = ((1000*(BlinkTimer.QuadPart - StartTime.QuadPart) / (BlinkMS*Frequency.QuadPart)) & 1);
        if(!Terminal->Renderer.Device)
        {
            Terminal->Renderer = AcquireD3D11Renderer(Terminal->Window, 0);
            RefreshFont(Terminal);
        }
        if(Terminal->Renderer.Device)
        {
            RendererDraw(Terminal, Width, Height, &Terminal->ScreenBuffer, Blink ? 0xffffffff : 0xff222222);
        }
        ++FrameIndex;
        ++FrameCount;

        LARGE_INTEGER Now;
        QueryPerformanceCounter(&Now);

        if (Now.QuadPart >= UpdateTitle)
        {
            UpdateTitle = Now.QuadPart + Frequency.QuadPart;

            double FramesPerSec = (double)FrameCount * Frequency.QuadPart / (Now.QuadPart - Time.QuadPart);
            Time = Now;
            FrameCount = 0;

            WCHAR Title[1024];

            if(Terminal->NoThrottle)
            {
                glyph_table_stats Stats = GetAndClearStats(Terminal->GlyphTable);
                wsprintfW(Title, L"refterm Size=%dx%d RenderFPS=%d.%02d CacheHits/Misses=%d/%d Recycle:%d",
                              Terminal->ScreenBuffer.DimX, Terminal->ScreenBuffer.DimY, (int)FramesPerSec, (int)(FramesPerSec*100) % 100,
                              (int)Stats.HitCount, (int)Stats.MissCount, (int)Stats.RecycleCount);
            }
            else
            {
                wsprintfW(Title, L"refterm");
            }

            SetWindowTextW(Terminal->Window, Title);
        }
    }

    DWriteRelease(&Terminal->GlyphGen);
    ReleaseD3D11Renderer(&Terminal->Renderer);

    // TODO(casey): How do we actually do an ensured-kill here?  Like even if we crash?  Is there some kind
    // of process parameter we can pass to CreateProcess that will ensure it is killed?  Because this won't.
    KillProcess(Terminal);

    ExitProcess(0);
}
