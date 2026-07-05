@echo off
rem Build with MSVC (Visual Studio 2022). Run from the repo root.
setlocal

where cl >nul 2>&1
if not errorlevel 1 goto have_cl

set "VSPATH="
for %%e in (Community Professional Enterprise BuildTools) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\2022\%%e"
)
if "%VSPATH%"=="" echo Visual Studio 2022 not found & exit /b 1
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
:have_cl

set CFLAGS=/nologo /O2 /std:c11 /utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS /Isrc

cl %CFLAGS% src\main.c src\plo5.c /Fe:plo5calc.exe
if errorlevel 1 exit /b 1
cl %CFLAGS% tests\tests.c src\plo5.c /Fe:plo5tests.exe
if errorlevel 1 exit /b 1
rc /nologo /fo plo5gui.res src\plo5gui.rc
if errorlevel 1 exit /b 1
cl %CFLAGS% src\gui.c src\plo5.c plo5gui.res /Fe:plo5gui.exe /link /subsystem:windows user32.lib gdi32.lib
if errorlevel 1 exit /b 1
cl %CFLAGS% src\quiz.c src\plo5.c /Fe:plo5quiz.exe /link /subsystem:windows user32.lib gdi32.lib
if errorlevel 1 exit /b 1
del *.obj *.res >nul 2>&1
echo Built plo5calc.exe, plo5tests.exe, plo5gui.exe and plo5quiz.exe
