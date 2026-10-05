@echo off
setlocal enabledelayedexpansion
rem Build the ASCIIArt desktop application.
rem   -municode  : the entry point is wWinMain (the Unicode form), which the
rem                linker will not resolve without this flag
rem   -mwindows  : GUI subsystem, so no console window is created

set STEPS=5
set N=0
echo.
echo   ASCIIArt - build
echo   ===============================================

rem ------------------------------------------------------------ 1. toolchain
set /a N+=1
echo   [!N!/%STEPS%] Locating the compiler
where gcc >nul 2>&1
if errorlevel 1 (
  echo         ERROR: gcc was not found on PATH.
  echo         Install MSYS2 or MinGW-w64 and try again.
  exit /b 1
)
for /f "tokens=*" %%v in ('gcc -dumpversion') do set GCCVER=%%v
echo         gcc !GCCVER!

rem ----------------------------------------------------------- 2. icon asset
set /a N+=1
if exist asciiapp.ico (
  echo   [!N!/%STEPS%] Application icon present, skipping generation
) else (
  echo   [!N!/%STEPS%] Generating the application icon
  python make_icon.py
  if errorlevel 1 echo         WARNING: icon generation failed, continuing without it
)

rem -------------------------------------------------------- 3. resource file
set /a N+=1
echo   [!N!/%STEPS%] Compiling resources
windres asciiapp.rc -O coff -o asciiapp_res.o 2>nul
if exist asciiapp_res.o (
  echo         asciiapp_res.o
  set RES=asciiapp_res.o
) else (
  echo         WARNING: windres unavailable, building without an icon
  set RES=
)

rem ----------------------------------------------------- 4. compile and link
set /a N+=1
echo   [!N!/%STEPS%] Compiling and linking asciiapp.c
echo         gcc -O2 -municode -mwindows -o asciiapp.exe asciiapp.c !RES!
gcc -O2 -municode -mwindows -o asciiapp.exe asciiapp.c !RES! -lgdi32 -lcomdlg32 -lshell32 -lm
if errorlevel 1 (
  echo.
  echo   BUILD FAILED
  exit /b 1
)

rem ---------------------------------------------------------- 5. verification
set /a N+=1
echo   [!N!/%STEPS%] Verifying output
if not exist asciiapp.exe (
  echo         ERROR: asciiapp.exe was not produced.
  exit /b 1
)
for %%f in (asciiapp.exe) do echo         asciiapp.exe  ^(%%~zf bytes^)

echo   ===============================================
echo   Build succeeded.
echo.
echo   Run:          asciiapp.exe samples\sphere.png
echo   Start menu:   powershell -ExecutionPolicy Bypass -File install-shortcut.ps1
echo.
