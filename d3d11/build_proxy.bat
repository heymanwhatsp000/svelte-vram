@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set SRCDIR=%~dp0
set OUTDIR=%~dp0build
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo === Compiling Svelte d3d11.dll ===
cd /d "%SRCDIR%"

cl.exe /nologo /O2 /MT /LD /EHsc /std:c++14 /D _CRT_SECURE_NO_WARNINGS /D _WIN32_WINNT=0x0A00 ^
    dllmain.cpp ^
    svelte_util.cpp ^
    svelte_strip.cpp ^
    svelte_registry.cpp ^
    svelte_wrapped_device.cpp ^
    svelte_wrapped_swapchain.cpp ^
    /Fe:"%OUTDIR%\d3d11.dll" /Fo:"%OUTDIR%\\" ^
    /link /DEF:"%SRCDIR%d3d11_proxy.def" d3d11.lib ole32.lib kernel32.lib user32.lib

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo BUILD SUCCESS
dir "%OUTDIR%\d3d11.dll"
