@echo off
REM NOTE(yigit): Builds two things into build\:
REM
REM   game.dll   everything in code\game.cpp - the game and the renderer.
REM              Rebuild it while clash.exe is running and the new code is
REM              swapped in on the next frame.
REM   clash.exe   the platform layer, code\win32_clash.cpp.  Only needs
REM              rebuilding when the platform changes - and clash.exe must
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
REM -DCLASH_DEBUG=1  development build: Assert, P, L, and the debug file services
set CompilerFlags=-MTd -nologo -Gm- -GR- -EHa- -Od -Oi -WX -W4 -wd4100 -wd4189 -wd4505 -wd4201 -FC -Z7 -DCLASH_DEBUG=1

if not exist build mkdir build
pushd build

REM How long each part takes to build, printed after it.  See :Centiseconds.
call :Centiseconds BuildStart

REM NOTE(yigit): The DLL's debug info gets a new, random name every build.
REM While clash.exe runs, the debugger holds the old .pdb open, and a build
REM that tried to overwrite it would fail.  The del clears out the old ones
REM that are no longer held.
del game_*.pdb >nul 2>nul

REM NOTE(yigit): lock.tmp exists for exactly as long as game.dll is being
REM written.  A running clash.exe does not reload while it is there - without
REM it, it can catch the linker halfway through, load a half-written DLL (or
REM fail to copy it), and then never notice the finished one.
echo WAITING FOR PDB > lock.tmp

REM The DirectX 12 Agility SDK's runtime and debug layer, next to clash.exe
REM in D3D12\ - where the D3D12SDKPath export in win32_clash.cpp points.  /D
REM copies only when ours are newer, so a running clash.exe (which holds them
REM open) does not make the build fail.
if not exist D3D12 mkdir D3D12
xcopy /Y /D /Q ..\code\external\D3D12AgilitySDK\bin\x64\*.dll D3D12\ >nul

REM The DirectX Shader Compiler, next to clash.exe, where LoadLibrary looks
REM first.  The game loads dxcompiler.dll itself, at run time - it is not
REM linked - so a hot reload of game.dll never unloads the compiler under
REM it.  dxil.dll is the validator that signs every compiled shader.
xcopy /Y /D /Q ..\code\external\dxc\bin\x64\dxcompiler.dll . >nul
xcopy /Y /D /Q ..\code\external\dxc\bin\x64\dxil.dll . >nul

REM The game.  -LD makes a DLL; -EXPORT names what the platform looks up.
REM The DirectX libraries are linked here, not into the exe - the platform
REM never touches DirectX.  -I puts the Agility SDK's d3d12.h ahead of the
REM Windows SDK's older one, so the headers match the runtime we ship.  The
REM second -I is Dear ImGui's folder: its backend says #include "imgui.h".
REM The third is DXC's, for dxcapi.h.
call :Centiseconds GameStart
cl %CompilerFlags% -I..\code\external\D3D12AgilitySDK\include -I..\code\external\imgui -I..\code\external\dxc\inc ..\code\game.cpp -Fegame.dll -Fmgame.map -LD /link -incremental:no -opt:ref -PDB:game_%random%.pdb -EXPORT:GameUpdateAndRender -EXPORT:GameGetSoundSamples -EXPORT:GameShutdown -EXPORT:GameUnload d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib
set GameResult=%ERRORLEVEL%
call :PrintElapsed "game.dll " GameStart

del lock.tmp

REM The platform.  ole32 is COM - CoCreateInstance and friends - which WASAPI,
REM the sound API, is built on.
call :Centiseconds PlatformStart
cl %CompilerFlags% ..\code\win32_clash.cpp -Feclash.exe -Fmclash.map /link -incremental:no -opt:ref -subsystem:windows user32.lib gdi32.lib ole32.lib
set PlatformResult=%ERRORLEVEL%
call :PrintElapsed "clash.exe" PlatformStart
call :PrintElapsed "total    " BuildStart

popd
popd

if %GameResult% neq 0 (
    echo.
    echo GAME DLL BUILD FAILED
    exit /b %GameResult%
)
if %PlatformResult% neq 0 (
    echo.
    echo PLATFORM BUILD FAILED  -  is clash.exe still running?
    exit /b %PlatformResult%
)
exit /b 0

REM ---------------------------------------------------------------------------
REM call :Centiseconds Name - sets the variable Name to the time of day in
REM hundredths of a second.  %TIME% is " 2:39:14.86" here, "02:39:14,86" in
REM some locales: the leading space becomes a 0, and ":", "." and "," all
REM split it.  "1%%a - 100" reads "08" as 8 - plain "08" would be octal, and
REM invalid.
:Centiseconds
set "Now=%TIME: =0%"
for /f "tokens=1-4 delims=:.," %%a in ("%Now%") do (
    set /a "%1=(((1%%a-100)*60 + (1%%b-100))*60 + (1%%c-100))*100 + (1%%d-100)"
)
exit /b 0

REM call :PrintElapsed "label" StartName - prints the label and the seconds
REM since StartName was set by :Centiseconds, e.g. "game.dll   2.47s".  A build
REM across midnight would go negative, so a day is added back.
:PrintElapsed
call :Centiseconds ElapsedEnd
set /a Elapsed=ElapsedEnd - %2
if %Elapsed% lss 0 set /a Elapsed+=8640000
set /a ElapsedSeconds=Elapsed / 100
set /a ElapsedHundredths=Elapsed %% 100
set "ElapsedHundredths=0%ElapsedHundredths%"
echo %~1  %ElapsedSeconds%.%ElapsedHundredths:~-2%s
exit /b 0
