@echo off
setlocal

where /q cl || (
  echo ERROR: "cl" not found - please run this from the MSVC x64 native tools command prompt.
  exit /b 1
)

if "%Platform%" neq "x64" (
    echo ERROR: Platform is not "x64" - please run this from the MSVC x64 native tools command prompt.
    exit /b 1
)

call fxc /nologo /T cs_5_0 /E ComputeMain /O3 /WX /Fh refterm_cs.h /Vn ReftermCSShaderBytes /Qstrip_reflect /Qstrip_debug /Qstrip_priv refterm.hlsl
call fxc /nologo /T ps_5_0 /E PixelMain /O3 /WX /Fh refterm_ps.h /Vn ReftermPSShaderBytes /Qstrip_reflect /Qstrip_debug /Qstrip_priv refterm.hlsl
call fxc /nologo /T vs_5_0 /E VertexMain /O3 /WX /Fh refterm_vs.h /Vn ReftermVSShaderBytes /Qstrip_reflect /Qstrip_debug /Qstrip_priv refterm.hlsl

set CFLAGS=/nologo /W3 /Z7 /GS- /Gs999999
set LDFLAGS=/incremental:no /opt:icf /opt:ref

set CLANGCompileFlags= -g -nostdlib -nostdlib++ -mno-stack-arg-probe -maes
set CLANGLinkFlags=-fuse-ld=lld -Wl,-subsystem:windows

set BASE_FILES=refterm.c refterm_example_dwrite.cpp

call cl -D_DEBUG -Od -Ferefterm_debug_msvc.exe %CFLAGS% %BASE_FILES% /link %LDFLAGS% /subsystem:windows
call cl -O2 -Ferefterm_release_msvc.exe %CFLAGS% %BASE_FILES% /link %LDFLAGS% /subsystem:windows

call cl -O2 -Fesplat.exe %CFLAGS% splat.cpp /link %LDFLAGS% /subsystem:console
call cl -O2 -Fesplat2.exe %CFLAGS% splat2.cpp /link %LDFLAGS% /subsystem:console

where /q clang || (
  echo WARNING: "clang" not found - to run the fastest version of refterm, please install CLANG.
  exit /b 1
)

call clang %CLANGCompileFlags% %CLANGLinkFlags% %BASE_FILES% -o refterm_debug_clang.exe
call clang -O3 %CLANGCompileFlags% %CLANGLinkFlags% %BASE_FILES% -o refterm_release_clang.exe
