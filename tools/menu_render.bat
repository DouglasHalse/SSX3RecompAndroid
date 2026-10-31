REM Compile and run the host-side renderer for the in-game options screen.
REM   tools\menu_render.bat   -> tests\out\menu_android_*.raw
@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0\.."
if not exist tests\out mkdir tests\out
del /q tests\out\ssx3.cfg 2>nul
cl /nologo /W3 /Fe:tests\out\menu_render.exe /Fo:tests\out\ ^
   tests\menu_render.c src\settings.c src\in_game_menu.c ..\xboxrecomp\src\video\ui_overlay.c ^
   /D_CRT_SECURE_NO_WARNINGS /D__ANDROID__ /I src /I ..\xboxrecomp\src /link xinput.lib user32.lib
if errorlevel 1 exit /b 1
set SSX3_MENU_RENDER=1
tests\out\menu_render.exe 2400 1080 tests\out
