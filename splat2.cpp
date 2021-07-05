#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <time.h>
#include <windows.h>

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

    HANDLE StdOut = GetStdHandle(STD_OUTPUT_HANDLE);

    double Elapsed = 0.0;
    size_t TotalTransfer = 0;
    size_t BufferSize = 64*1024*1024;
    char *Buffer = (char *)malloc(BufferSize);
    if(Buffer)
    {
        if(ArgCount <= 1)
        {
            for(size_t At = 0; At < BufferSize; ++At)
            {
                int Pick = rand()%28;
                Buffer[At] = 'a' + Pick;;
                if(Pick == 27) Buffer[At] = '\n';
            }

            clock_t Start = clock();
            while(TotalTransfer < 1024*1024*1024)
            {
                DWORD ByteCount = 0;
                WriteFile(StdOut, Buffer, (DWORD)BufferSize, &ByteCount, 0);
                TotalTransfer += ByteCount;
            }
            clock_t End = clock();
            Elapsed += (double)(End - Start) / (double)CLOCKS_PER_SEC;
        }
        else
        {
            for(int ArgIndex = 1;
                ArgIndex < ArgCount;
                ++ArgIndex)
            {
                char *FileName = Args[ArgIndex];
                HANDLE File = CreateFileA(FileName, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
                                              FILE_ATTRIBUTE_NORMAL, 0);
                if(File != INVALID_HANDLE_VALUE)
                {
                    clock_t Start = clock();
                    DWORD ByteCount = 0;
                    while(ReadFile(File, Buffer, (DWORD)BufferSize, &ByteCount, 0) && ByteCount)
                    {
                        WriteFile(StdOut, Buffer, (DWORD)ByteCount, &ByteCount, 0);
                        TotalTransfer += ByteCount;
                    }
                    clock_t End = clock();
                    Elapsed += (double)(End - Start) / (double)CLOCKS_PER_SEC;

                    CloseHandle(File);
                }
                else
                {
                    fprintf(stderr, "Unable to open \"%s\".\n", FileName);
                }
            }
        }

        fprintf(stdout, "\n\nTotal sink time: %.03fs (%fgb/s)\n",
                Elapsed, TotalTransfer / (1024.0*1024.0*1024.0*Elapsed));

        free(Buffer);
    }

    return 0;
}
