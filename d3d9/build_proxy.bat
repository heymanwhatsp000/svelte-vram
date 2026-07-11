@echo off
setlocal
:: D3D9 build - must compile as 32-bit (x86) for FNV/SkyrimLE/FO3
:: Uses vcvars32 (not vcvars64) to get the 32-bit cl.exe
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
set SRCDIR=%~dp0
set OUTDIR=%~dp0build
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo === Compiling Svelte d3d9.dll (32-bit x86) ===
cd /d "%SRCDIR%"

cl.exe /nologo /O2 /MT /LD /EHsc /std:c++14 /D _CRT_SECURE_NO_WARNINGS /D _WIN32_WINNT=0x0601 ^
    dllmain.cpp ^
    svelte_util.cpp ^
    svelte_strip.cpp ^
    svelte_registry.cpp ^
    svelte_wrapped_device.cpp ^
    svelte_wrapped_d3d9.cpp ^
    svelte_wrapped_texture.cpp ^
    /Fe:"%OUTDIR%\d3d9.dll" /Fo:"%OUTDIR%\\" ^
    /link /DEF:"%SRCDIR%d3d9_proxy.def" d3d9.lib ole32.lib kernel32.lib user32.lib

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo BUILD SUCCESS
dir "%OUTDIR%\d3d9.dll"