@echo off
rem ===========================================================================
rem  TR2013 HUD Fix+ (TR_HUD_Fix) - build script (32-bit x86, MSVC, static CRT /MT)
rem
rem  Usage:  build.bat           build build\d3d11.dll
rem          build.bat deploy    build, then copy d3d11.dll + tr_hud_fix.ini into
rem                              the game folder (env TR_GAME_DIR or build.local.cmd)
rem          build.bat release   build, then assemble release\ + zip (logging off)
rem
rem  Never links d3d11.lib / dxgi.lib (the proxy would import itself).
rem ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0"
set "ROOT=%~dp0"
set "MODE=%~1"
set "PF86=%ProgramFiles(x86)%"
set "PF64=%ProgramFiles%"
if not defined PF86 set "PF86=%ProgramFiles%"

rem ---------------------------------------------------------------------------
rem  1. Toolchain: use cl.exe from PATH only if it already targets x86
rem ---------------------------------------------------------------------------
where cl.exe >nul 2>&1
if errorlevel 1 goto :find_vcvars
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" goto :have_toolchain

:find_vcvars
set "VCVARS="
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :vcvars_fallback
for /f "usebackq tokens=* delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call :try_vs "%%i"

:vcvars_fallback
if defined VCVARS goto :call_vcvars
for %%V in (2022 2019) do for %%E in (Community Professional Enterprise BuildTools) do call :try_vs "%PF64%\Microsoft Visual Studio\%%V\%%E"
for %%V in (2022 2019) do for %%E in (Community Professional Enterprise BuildTools) do call :try_vs "%PF86%\Microsoft Visual Studio\%%V\%%E"
if defined VCVARS goto :call_vcvars
echo [ERROR] No Visual Studio 2019/2022 C++ toolchain (vcvars32.bat) found.
echo         Install "Desktop development with C++" or run from an
echo         "x86 Native Tools Command Prompt for VS".
exit /b 1

:call_vcvars
echo [INFO] Using "%VCVARS%"
call "%VCVARS%" >nul
cd /d "%ROOT%"
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo [ERROR] vcvars32.bat did not provide cl.exe.
    exit /b 1
)
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" (
    echo [ERROR] Toolchain does not target x86 ^(VSCMD_ARG_TGT_ARCH=%VSCMD_ARG_TGT_ARCH%^).
    exit /b 1
)

:have_toolchain
echo [INFO] Toolchain target: %VSCMD_ARG_TGT_ARCH%

rem ---------------------------------------------------------------------------
rem  2. Build the proxy
rem ---------------------------------------------------------------------------
if not exist "build\obj_proxy" mkdir "build\obj_proxy"
if errorlevel 1 goto :build_failed
if not exist "build\obj_imgui" mkdir "build\obj_imgui"
if errorlevel 1 goto :build_failed

set "IMGUI_DIR=third_party\imgui"
if not exist "%IMGUI_DIR%\imgui.cpp" (
    echo [ERROR] Dear ImGui sources not found in %IMGUI_DIR%
    exit /b 2
)

rem Dear ImGui (third-party, unchanged).
set "IMGUI_FLAGS=/nologo /c /O2 /MT /std:c++17 /EHsc /W3 /utf-8 /DWIN32 /D_WINDOWS /DNDEBUG /DUNICODE /D_UNICODE /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD /I%IMGUI_DIR% /I%IMGUI_DIR%\backends"
echo.
echo [BUILD] Dear ImGui -^> build\obj_imgui
cl %IMGUI_FLAGS% %IMGUI_DIR%\imgui.cpp %IMGUI_DIR%\imgui_draw.cpp %IMGUI_DIR%\imgui_tables.cpp %IMGUI_DIR%\imgui_widgets.cpp %IMGUI_DIR%\backends\imgui_impl_dx11.cpp %IMGUI_DIR%\backends\imgui_impl_win32.cpp /Fo"build\obj_imgui\\"
if errorlevel 1 goto :build_failed
set "IMGUI_OBJS=build\obj_imgui\imgui.obj build\obj_imgui\imgui_draw.obj build\obj_imgui\imgui_tables.obj build\obj_imgui\imgui_widgets.obj build\obj_imgui\imgui_impl_dx11.obj build\obj_imgui\imgui_impl_win32.obj"

rem Our code: /W4. d3dcompiler_47/dwmapi/shell32 (pulled in by ImGui) are
rem delay-loaded so they are only touched when the menu is first opened.
set "CFLAGS=/nologo /O2 /MT /std:c++17 /EHsc /W4 /utf-8 /DWIN32 /D_WINDOWS /DNDEBUG /DUNICODE /D_UNICODE /I%IMGUI_DIR% /I%IMGUI_DIR%\backends"
set "LIBS=user32.lib gdi32.lib d3dcompiler.lib delayimp.lib /DELAYLOAD:d3dcompiler_47.dll /DELAYLOAD:dwmapi.dll /DELAYLOAD:shell32.dll"

echo.
echo [BUILD] build\d3d11.dll  (proxy)
cl %CFLAGS% /DTRHF_PROXY /LD src\dllmain.cpp %IMGUI_OBJS% /Fo"build\obj_proxy\\" /Fe"build\d3d11.dll" /link /MACHINE:X86 /DEF:src\exports.def %LIBS%
if errorlevel 1 goto :build_failed

echo.
echo [OK] Build finished: build\d3d11.dll

if /i "%MODE%"=="release" goto :release
if /i not "%MODE%"=="deploy" goto :done

rem ---------------------------------------------------------------------------
rem  3. Deploy (only d3d11.dll + tr_hud_fix.ini; never overwrite foreign DLLs)
rem ---------------------------------------------------------------------------
rem Game folder: env TR_GAME_DIR, or a local, git-ignored build.local.cmd
rem containing e.g.:  set "TR_GAME_DIR=C:\Games\Tomb Raider"
if not defined TR_GAME_DIR if exist "%~dp0build.local.cmd" call "%~dp0build.local.cmd"
if not defined TR_GAME_DIR goto :deploy_no_cfg
set "GAME_DIR=%TR_GAME_DIR%"
echo.
echo [DEPLOY] Game folder: "%GAME_DIR%"

if not exist "%GAME_DIR%\" goto :deploy_no_dir
if not exist "%GAME_DIR%\TombRaider.exe" goto :deploy_no_exe
if not exist "%GAME_DIR%\d3d11.dll" goto :deploy_copy
findstr /m /c:"TR_HUD_Fix_Marker_v1" "%GAME_DIR%\d3d11.dll" >nul 2>&1
if errorlevel 1 goto :deploy_foreign

:deploy_copy
copy /y "build\d3d11.dll" "%GAME_DIR%\d3d11.dll" >nul
if errorlevel 1 goto :deploy_copy_failed
copy /y "tr_hud_fix.ini" "%GAME_DIR%\tr_hud_fix.ini" >nul
if errorlevel 1 goto :deploy_copy_failed
echo [OK] Deployed d3d11.dll and tr_hud_fix.ini to "%GAME_DIR%"
goto :done

rem ---------------------------------------------------------------------------
rem  4. Release package (release\TR2013_HUD_Fix_Plus\ +
rem     release\TR2013_HUD_Fix_Plus_v<ver>.zip)
rem ---------------------------------------------------------------------------
:release
set "REL=release\TR2013_HUD_Fix_Plus"
if exist "release" rmdir /s /q "release"
mkdir "%REL%\licenses" || goto :release_failed
copy /y "build\d3d11.dll" "%REL%\d3d11.dll" >nul || goto :release_failed
copy /y "README.txt" "%REL%\README.txt" >nul || goto :release_failed
copy /y "LICENSE" "%REL%\licenses\TR_HUD_Fix_LICENSE.txt" >nul || goto :release_failed
copy /y "third_party\imgui\LICENSE.txt" "%REL%\licenses\DearImGui_LICENSE.txt" >nul || goto :release_failed
rem End-user ini: analysis logging off (also disables the F10 capture).
powershell -NoProfile -Command "$t=[IO.File]::ReadAllText('tr_hud_fix.ini') -replace '(?m)^EnableLogging=1','EnableLogging=0'; [IO.File]::WriteAllText('%REL%\tr_hud_fix.ini',$t,[Text.Encoding]::ASCII)" || goto :release_failed
findstr /r /c:"^EnableLogging=0" "%REL%\tr_hud_fix.ini" >nul || goto :release_failed
for /f tokens^=2^ delims^=^" %%v in ('findstr /c:"kVersion =" src\dllmain.cpp') do set "VER=%%v"
rem tar.exe (Windows 10+) writes zip entries with "/" separators, so the zip
rem also extracts correctly on Linux / Steam Deck (Compress-Archive may use "\").
"%SystemRoot%\System32\tar.exe" -a -c -f "release\TR2013_HUD_Fix_Plus_v%VER%.zip" -C release TR2013_HUD_Fix_Plus || goto :release_failed
echo [OK] Release: %REL%\  and  release\TR2013_HUD_Fix_Plus_v%VER%.zip
goto :done

:release_failed
echo [ERROR] Release packaging failed.
exit /b 5

:deploy_no_cfg
echo [ERROR] No game folder configured. Set TR_GAME_DIR or create build.local.cmd
echo         with:  set "TR_GAME_DIR=C:\path\to\Tomb Raider"
exit /b 3

:deploy_no_dir
echo [ERROR] Game folder not found: "%GAME_DIR%" (check TR_GAME_DIR)
exit /b 3

:deploy_no_exe
echo [ERROR] TombRaider.exe not found in "%GAME_DIR%"
exit /b 3

:deploy_foreign
echo [ERROR] "%GAME_DIR%\d3d11.dll" exists and is NOT TR2013 HUD Fix+ (DXVK/ReShade/other?).
echo         Aborting - foreign DLLs are never overwritten. Rename it to
echo         d3d11_orig.dll (auto chain-loading).
exit /b 4

:deploy_copy_failed
echo [ERROR] Copy failed (game running? missing write permission?)
exit /b 5

:build_failed
echo.
echo [ERROR] Build failed.
exit /b 2

:done
exit /b 0

rem ---------------------------------------------------------------------------
rem  Subroutine: accept a VS installation root if it contains vcvars32.bat
rem ---------------------------------------------------------------------------
:try_vs
if defined VCVARS goto :eof
if exist "%~1\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%~1\VC\Auxiliary\Build\vcvars32.bat"
goto :eof
