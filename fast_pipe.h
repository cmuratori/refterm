/* NOTE(casey):

   "Fast pipe" is a technique to bypass the (very slow) Windows conio subsystem.
   Since the Windows kernel has reasonably fast pipes, if the calling process
   opens a named pipe under the "fastpipe########" label, where the #'s are
   replaced with our process ID, then we know that the shell on the other
   side can accept direct output.  stdin and stdout are then remapped to
   that pipe.
   
   To use, #include this file in your program, and insert
   
       USE_FAST_PIPE_IF_AVAILABLE();
       
   at the very start of your program (ideally the first line in main).
   It will also optionally return non-zero when the fast pipe is available,
   in case your program wishes to take special action when a fast pipe
   exists:
   
       int FastPipeIsAvailable = USE_FAST_PIPE_IF_AVAILABLE();
       
   No further changes to the program should be necessary.  Note that
   stderr is, by default, still mapped to the slow conio pipe.
*/

#if _WIN32
#include <windows.h>
#ifndef _VC_NODEFAULTLIB
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#endif
#pragma comment (lib, "kernel32.lib")
#pragma comment (lib, "user32.lib")
static int USE_FAST_PIPE_IF_AVAILABLE()
{
    int Result = 0;
    
    wchar_t PipeName[32];
    wsprintfW(PipeName, L"\\\\.\\pipe\\fastpipe%x", GetCurrentProcessId());
    HANDLE FastPipe = CreateFileW(PipeName, GENERIC_READ|GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    if(FastPipe != INVALID_HANDLE_VALUE)
    {
        SetStdHandle(STD_OUTPUT_HANDLE, FastPipe);
        SetStdHandle(STD_INPUT_HANDLE, FastPipe);
        
#ifndef _VC_NODEFAULTLIB
        int StdOut = _open_osfhandle((intptr_t)FastPipe, O_WRONLY|O_TEXT);
        int StdIn = _open_osfhandle((intptr_t)FastPipe, O_RDONLY|O_TEXT);
        
        _dup2(StdOut, _fileno(stdout));
        _dup2(StdIn, _fileno(stdin));
        
        _close(StdOut);
        _close(StdIn);
#endif

        Result = 1;
    }
    
    return Result;
}
#else
#define USE_FAST_PIPE_IF_AVAILABLE(...) 0
#endif
