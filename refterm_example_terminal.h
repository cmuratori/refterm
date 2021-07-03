enum
{
    TerminalCell_Bold = 0x2,
    TerminalCell_Dim = 0x4,
    TerminalCell_Italic = 0x8,
    TerminalCell_Underline = 0x10,
    TerminalCell_Blinking = 0x20,
    TerminalCell_ReverseVideo = 0x40,
    TerminalCell_Invisible = 0x80,
    TerminalCell_Strikethrough = 0x100,
};

typedef struct
{
    renderer_cell *Cells;
    uint32_t DimX, DimY;
    uint32_t FirstLineY;
} terminal_buffer;

typedef struct
{
    int32_t X, Y;
} terminal_point;

typedef struct
{
    uint32_t Foreground;
    uint32_t Background;
    uint32_t Flags;
} glyph_props;

typedef struct
{
    terminal_point At;
    glyph_props Props;
} cursor_state;

typedef struct
{
    // TODO(casey): Get rid of Uniscribe so this garbage doesn't have to happen
    
    SCRIPT_DIGITSUBSTITUTE UniDigiSub;
    SCRIPT_CONTROL UniControl;
    SCRIPT_STATE UniState;
    SCRIPT_CACHE UniCache;

    wchar_t Expansion[1024];
    SCRIPT_ITEM Items[1024];
    SCRIPT_LOGATTR Log[1024];
    DWORD SegP[1026];
} example_partitioner;

typedef struct
{
    size_t FirstP;
    size_t OnePastLastP;
    uint32_t ContainsComplexChars;
    glyph_props StartingProps;
} example_line;

typedef struct
{
    HWND Window;
    int Quit;
    
    d3d11_renderer Renderer;
    glyph_generator GlyphGen;
    void *GlyphTableMem;
    glyph_table *GlyphTable;
    terminal_buffer ScreenBuffer;
    source_buffer ScrollBackBuffer;
    example_partitioner Partitioner;
    
    HANDLE Legacy_WriteStdIn;
    HANDLE Legacy_ReadStdOut;
    HANDLE Legacy_ReadStdError;

    int EnableFastPipe;
    HANDLE FastPipe;
    
    HANDLE ChildProcess;
 
    cursor_state RunningCursor;
    
    uint32_t CommandLineCount;
    char CommandLine[256];
    
    int DebugHighlighting;
    
    uint32_t MaxLineCount;
    uint32_t CurrentLineIndex;
    uint32_t LineCount;
    example_line *Lines;
    
    int32_t ViewingLineOffset;
    
    wchar_t RequestedFontName[64];
    uint32_t RequestedFontHeight;
    int RequestClearType;
    int RequestDirectWrite;
    int LineWrap;
    
    uint32_t REFTERM_TEXTURE_WIDTH;
    uint32_t REFTERM_TEXTURE_HEIGHT;
    
    uint32_t TransferWidth;
    uint32_t TransferHeight;
    
    uint32_t REFTERM_MAX_WIDTH;
    uint32_t REFTERM_MAX_HEIGHT;

#define MinDirectCodepoint 32
#define MaxDirectCodepoint 126
    gpu_glyph_index ReservedTileTable[MaxDirectCodepoint - MinDirectCodepoint + 1];
} example_terminal;

