#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <time.h>

#include "fast_pipe.h"

int main(int ArgCount, char **Args)
{
    int FastPipeAvailable = USE_FAST_PIPE_IF_AVAILABLE();

#if _WIN32
    if(!FastPipeAvailable)
    {
        // NOTE(casey): Need to set UTF-8 output page if we are using the antiquated Windows terminal system
        SetConsoleOutputCP(65001);
    }
#endif
    
    size_t TotalTransfer = 0;
    size_t BufferSize = 64*1024*1024;
    char *Buffer = (char *)malloc(BufferSize);
    if(Buffer)
    {
        for(int ArgIndex = 1;
            ArgIndex < ArgCount;
            ++ArgIndex)
        {
            char *FileName = Args[ArgIndex];
            FILE *File = fopen(FileName, "rb");
            if(File)
            {
                clock_t Start = clock();
                while(size_t ByteCount = fread(Buffer, 1, BufferSize, File))
                {
                    TotalTransfer += ByteCount;
                    fwrite(Buffer, 1, ByteCount, stdout);
                }
                clock_t End = clock();
                
                double Elapsed = (double)(End - Start) / (double)CLOCKS_PER_SEC;
                fprintf(stdout, "\n\nTotal sink time: %.03fs (%fgb/s)\n", 
                        Elapsed, TotalTransfer / (1024.0*1024.0*1024.0*Elapsed));
                
                fclose(File);
            }
            else
            {
                fprintf(stderr, "Unable to open \"%s\".\n", FileName);
            }
        }
     
        free(Buffer);
    }
    
    return 0;
}
