@echo off
setlocal

where /q cl.exe || (
  echo ERROR: please run this from MSVC command prompt
  exit /b 1
)

fxc.exe /nologo /T cs_5_0 /E shader /O3 /WX /Fh refterm_shader.h /Vn ReftermShaderBytes /Qstrip_reflect /Qstrip_debug /Qstrip_priv refterm.hlsl

set CFLAGS=/nologo /W3 /Z7 /GS- /Gs999999
set LDFLAGS=/incremental:no /opt:icf /opt:ref /subsystem:windows

cl.exe %CFLAGS% refterm.c /link %LDFLAGS%
