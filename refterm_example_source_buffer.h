typedef struct
{
    size_t AbsoluteP;
    size_t Count;
    char *Data;
} source_buffer_range;

typedef struct 
{
    size_t DataSize;
    char *Data;

    // NOTE(casey): For circular buffer
    size_t RelativePoint;
    
    // NOTE(casey): For cache checking
    size_t AbsoluteFilledSize;
} source_buffer;

