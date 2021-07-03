# refterm v1

refterm is a reference renderer for monospace terminal displays.  It was designed to demonstrate that even in the worst-case scenario - extremely slow Unicode parsing with Uniscribe and extremely slow glyph generation with DirectWrite - it is still straightforward to achieve reasonable frame rates and reasonable throughput by being sensible.

# Simple Code, Reasonable Speed

refterm actually isn't very fast.  Despite being several orders of magnitude faster than Windows Terminal, __refterm is largely unoptimized and is much slower than it could be__.  It is nothing more than a straightforward implementation of a tile renderer, with a very simple cache to ensure that glyph generation only gets called when new glyphs are seen.  It is all very, very simple.  A more complex codebase that parsed Unicode and rendered glyphs itself would likely be _much_ faster than refterm for many important metrics.

Similarly, VT code and line break parsing in refterm is completely unoptimized.  Total throughput could likely be improved dramatically by optimizing these routines.

TL;DR: refterm should be thought of as establishing a modern _minimum_ speed at which a reasonable terminal should be expected to perform.  It should not be thought of as a maximum to aspire to.  If your terminal runs _slower_ than refterm, that is not a good sign.

# Feature Support

refterm is designed to support several features, just to ensure that no shortcuts have been taken in the design of the renderer.  As such, refterm supports:

* Multicolor fonts
* All of Unicode, including combining characters and right-to-left text like Arabic
* Glyphs that can take up several cells
* Line wrapping
* Reflowing line wrapping on terminal resize
* Large scrollback buffer
* VT codes for setting colors and cursor positions

These features are not designed to be comprehensive, since this is only meant to be a reference renderer, not a complete terminal.

# Code Layout

The important code for the reference renderer resides in three simple files:

* refterm.hlsl - shader for doing tile-based rendering
* refterm_glyph_cache.h/c - cache for mapping Unicode runs to glyphs

The rest of the code - refterm_example_*.h/c - is just there to verify that the API for the glyph cache conveniently supports all the features a terminal needs.  The code in those files may be useful as a vague reference, but no thought was put into their design so it is not likely to be directly useful.

# Fast Pipes

Because Windows' has very serious problems with conio throughput, a conio bypass is included in refterm.  This was necsesary in order to test bandwidth through the terminal, since without bypassing Windows' console conduit, it quickly becomes the bottleneck.

_fast_pipe.h_ is a header file that provides a single macro you can use in any console program to remap the standard handles to faster handles that refterm provides.  You can look at the included _splat.c_ console example program to see how it is used.  Fast pipes are off by default in refterm, but can be turned on by typing "fastpipe" as a command.

In general, fast pipes are around 10x the speed of Windows' console conduit.  It is likely that they could run substantially faster, but refterm itself does not have highly optimized ingress code, so further work will need to be done to see what the upper bound on fast_pipe.h throughput is.

# Ongoing Work

There are some remaining issues that would be interesting to pursue.

* The current HLSL renderer doesn't support oversized glyphs (glyphs that extend outside the boundaries of their cells).  For a slight additional per-pixel cost, it would be straightforward to allow oversized glyphs.  This seems worthwhile to do, since font choices like italics would make it necessary to allow glyphs to lean into cells next to them, etc.
* It is not difficult to implement subpixel rendering (like ClearType) in a pixel shader like the one in refterm, but it would depend on the glyph generation being capable of providing subpixel rendering information.  If at some point refterm included a custom glyph generator, it would be worth putting in explicit subpixel support.  At the moment, ClearType can be used, but it would not necessarily produce the right colors against various backgrounds, etc.
