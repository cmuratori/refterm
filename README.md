# refterm v2

refterm is a reference renderer for monospace terminal displays.  It was designed to demonstrate that even in the worst-case scenario - extremely slow Unicode parsing with Uniscribe and extremely slow glyph generation with DirectWrite - it is still straightforward to achieve reasonable frame rates and reasonable throughput by being sensible.

__Please note that refterm is UTF-8__.  If you are doing tests with it, you must use UTF-8 encoded files or command output.

# Simple Code, Reasonable Speed

refterm actually isn't very fast.  Despite being several orders of magnitude faster than Windows Terminal, __refterm is largely unoptimized and is much slower than it could be__.  It is nothing more than a straightforward implementation of a tile renderer, with a very simple cache to ensure that glyph generation only gets called when new glyphs are seen.  It is all very, very simple.  A more complex codebase that parsed Unicode and rendered glyphs itself would likely be _much_ faster than refterm for many important metrics.

VT code and line break parsing in refterm has had one piece of optimization in v2.  It now checks each 16-byte block for control codes before actually running the parser.  This was done to help test high-bandwidth throughput.  The actual control code parser has still not been optimized at all, however.

TL;DR: refterm should be thought of as establishing a modern _minimum_ speed at which a reasonable terminal should be expected to perform.  It should not be thought of as a maximum to aspire to.  If your terminal runs _slower_ than refterm, that is not a good sign.

# Feature Support

refterm is designed to support several features, just to ensure that no shortcuts have been taken in the design of the renderer.  As such, refterm supports:

* Multicolor fonts
* All of Unicode, including combining characters and right-to-left text like Arabic
* Glyphs that can take up several cells
* Line wrapping
* Reflowing line wrapping on terminal resize
* Large scrollback buffer
* VT codes for setting colors and cursor positions, as well as strikethrough, underline, blink, reverse video, etc.

These features are not designed to be comprehensive, since this is only meant to be a reference renderer, not a complete terminal.

# Code Layout

The important code for the reference renderer resides in three simple files:

* refterm.hlsl - shader for doing tile-based rendering
* refterm_glyph_cache.h/c - cache for mapping Unicode runs to glyphs

The rest of the code - refterm_example_*.h/c - is just there to verify that the API for the glyph cache conveniently supports all the features a terminal needs.  The code in those files may be useful as a vague reference, but no thought was put into their design so it is not likely to be directly useful.

# Fast Pipes

Prior to v2, it was assumed that conhost had to be bypassed in order to do high-throughput terminal processing.  However, after some testing it was determined that so long as conhost receives large writes, it is actually within 10% of the fast pipe alternative.  So it _appears_ that so long as you either directly call WriteFile with large buffers, or you use stdio with large buffers and set stdout to binary mode, you _can_ get reasonable speed going through conhost!

