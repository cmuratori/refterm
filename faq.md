For people who know performance programming well, here are the things they might care about:

## There are two separate problems to talk about with respect to terminal speed on Windows.

One is the speed of the renderer, and the other is the speed of the pipe.  In Windows Terminal they are both slow, but the degree to which they are bad depends on how you are using it.

## The Windows Terminal renderer is slow itself. 

I have not done profiling of Windows Terminal directly, so there may be many sources of slowdown, take these with a grain of salt.  But at least two have been identified.  One is that it uses a standard "modern C++" approach to things with lots of indiscriminate use of things like std::vector and std::string, so even though there is no reason to ever allocate or deallocate anything when rendering a terminal, there is tons of that (and tons of call stacking, etc.) happening in the code base.  So the code in general does way, way more work than it needs to.

But that is likely only the reason it is slow when it is rendering single-color text.  The reason Windows Terminal gets slow when rendering multicolor text (like text where the color of the foreground and background changes frequently) is because there is no "renderer" per se in Windows Terminal, there is just a call to DirectWrite.  It calls DirectWrite as frequently as _once per character on the screen_ if it does not detect that a group of characters can be passed together.  Changing the background color is one of the (many) things that can prevent characters from being passed together, for example.  Obviously, a full call into DirectWrite every time you want to render a single character is going to put a signficant bound on how fast the terminal can update, no matter how optimized you made the rest of the code.

So the renderer in Windows Terminal is either slow (single color) or very slow (multiple background colors), before considering the pipe.

## The console pipe in Windows is slow separately.

Whenever you call CreateProcess() or otherwise cause a process to be created that uses /SUBSYSTEM:console, Windows inserts a thing called "conhost" in between the parent and the child process that intercepts all three standard handles (in, out, and error).  So communication between a terminal and a sub-process __is not just a pipe__.  It is actually a man-in-the-middle pipe with Windows doing a bunch of processing.

The reason for this architecture is because Windows supports a bunch of additional console calls (for example, https://docs.microsoft.com/en-us/windows/console/readconsoleoutputcharacter) which it obviously couldn't fulfill if it didn't buffer the output to the console itself.  So unlike a normal Windows pipe, a console pipe is artificially slow because it is intermediated by some code that is itself slow.

In the refterm demo, I attempted to "measure" this by showing the same code running with and without conhost as an intermediary.  I did this using a technique I called "fast pipes" (which you can see here: https://github.com/cmuratori/refterm/blob/main/fast_pipe.h).  All it does is use the process's ID to generate a unique named pipe with the Windows kernel that both the terminal and the child process then use in lieu of the intermediated handles they were given.

There is also a way in Windows 10 to do a limited bypass of conhost that allows a more complete terminal experience.  This is called ConPTY (https://github.com/microsoft/terminal/tree/main/src/winconpty) and it allows you to reimplement all the Windows conhost functions.  This is what you would need to do if you wanted to do a completely backwards-compatible terminal, so fast pipes is just a tool for testing.

## How slow is each?

In the demo video (https://www.youtube.com/watch?v=hxM8QmyZXtg), I show comparisons between dumping a one gigabyte file to Windows Terminal Preview (their most recent release) through conhost, which takes on the order of 330 seconds; dumping to refterm through conhost, which takes on the order of 40 seconds, and dumping to refterm through fast pipes, which takes on the order of 6 seconds.

This pretty clearly demonstrates that Windows Terminal is 10x slower than it should be, and conhost is another 10x slower than it should be :)

But this was with completely unoptimized code (as I mentioned in the video).  So when I showed that demo, I didn't know how much of the 6 seconds was just my code being bad.  Later that weekend, I found some time to put in the most basic optimization I could think of, and the no-conhost refterm speed went down to around 0.6 seconds, another order of magnitude.

But there was more intrique.  In the course of doing so, I found that actually, __how you call conhost matters a great deal__.  From preliminary inspection, it _appears_ as if the number of _times_ you call conhost is the problem, at least for non-VT-code output.  This makes reasonable sense, since the handoff might be costly as it is two separate processes.  But what obscures this is the fact that using stdio _introduces_ lots of extra conhost calls, because if you do not turn text mode off, it will use small buffers and generate a lot more calls to conhost.

So, if you either a) use WriteFile() directly, or b) set stdio mode to binary, you will actually find that conhost adds only a small ~10% overhead to the processing, rather than 10x.

## What about VT-codes?

Unfortunately, conhost seems to still be relatively slow when VT-codes are turned on.  By default, Windows doesn't handle VT-codes - you have to call SetConsoleMode() with a new flag in order to enable them.  Once you do that, conhost is much slower, presumably because it is now going through "modern" code that does a ton of work it doesn't need to do.

## That's probably not all.

This was a very spare-time project, which obviously I don't get paid to do.  There's a lot more to learn here, but those are the basic things that have been determined so far.

Hope that helps explain things.
