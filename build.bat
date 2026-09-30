@echo off
REM NOTE(yigit): Builds two things into build\:
REM
REM   game.dll   everything in code\game.cpp - the game and the renderer.
REM              Rebuild it while dx12.exe is running and the new code is
REM              swapped in on the next frame.
REM   dx12.exe   the platform layer, code\win32_dx12.cpp.  Only needs
REM              rebuilding when the platform changes - and dx12.exe must
REM              not be running then, or Windows will not let it be replaced.
REM
REM Works from any directory - %~dp0 is the folder this script lives in.
pushd "%~dp0"

REM Find the compiler, if this is not already a Visual Studio command prompt.
where cl >nul 2>nul
if errorlevel 1 (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>nul
)
where cl >nul 2>nul
if errorlevel 1 (
    echo ERROR: cl.exe not found.  Fix the vcvarsall.bat path in build.bat.
    popd
    exit /b 1
)

REM -Od          no optimisation - the debugger shows every variable
REM -Z7          debug information, for breakpoints
REM -W4 -WX      warnings on, and treated as errors
REM -wd4100      ...except "unused parameter" - the renderer starts as stubs
REM -wd4189      ...and "unused local variable", while things are half-written
REM -wd4505      ...and "unused function"
REM -wd4201      ...and "nameless struct/union", which the input structs use
REM -GR- -EHa-   no C++ run-time type info, no exceptions: neither is used
REM -MTd         the C runtime linked in, so nothing extra has to be installed
REM -DDX12_DEBUG=1  development build: Assert, P, L, and the debug file services
set CompilerFlags=-MTd -nologo -Gm- -GR- -EHa- -Od -Oi -WX -W4 -wd4100 -wd4189 -wd4505 -wd4201 -FC -Z7 -DDX12_DEBUG=1

if not exist build mkdir build
pushd build

REM NOTE(yigit): The DLL's debug info gets a new, random name every build.
REM While dx12.exe runs, the debugger holds the old .pdb open, and a build
REM that tried to overwrite it would fail.  The del clears out the old ones
REM that are no longer held.
del game_*.pdb >nul 2>nul

REM NOTE(yigit): lock.tmp exists for exactly as long as game.dll is being
REM written.  A running dx12.exe does not reload while it is there - without
REM it, it can catch the linker halfway through, load a half-written DLL (or
REM fail to copy it), and then never notice the finished one.
echo WAITING FOR PDB > lock.tmp

REM The game.  -LD makes a DLL; -EXPORT names what the platform looks up.
REM The DirectX libraries are linked here, not into the exe - the platform
REM never touches DirectX.
cl %CompilerFlags% ..\code\game.cpp -Fegame.dll -Fmgame.map -LD /link -incremental:no -opt:ref -PDB:game_%random%.pdb -EXPORT:GameUpdateAndRender -EXPORT:GameGetSoundSamples -EXPORT:GameShutdown d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib
set GameResult=%ERRORLEVEL%

del lock.tmp

REM The platform.  ole32 is COM - CoCreateInstance and friends - which WASAPI,
REM the sound API, is built on.
cl %CompilerFlags% ..\code\win32_dx12.cpp -Fedx12.exe -Fmdx12.map /link -incremental:no -opt:ref -subsystem:windows user32.lib gdi32.lib ole32.lib
set PlatformResult=%ERRORLEVEL%

popd
popd

if %GameResult% neq 0 (
    echo.
    echo GAME DLL BUILD FAILED
    exit /b %GameResult%
)
if %PlatformResult% neq 0 (
    echo.
    echo PLATFORM BUILD FAILED  -  is dx12.exe still running?
    exit /b %PlatformResult%
)
exit /b 0
