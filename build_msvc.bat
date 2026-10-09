@echo off
rem Build with Visual Studio. Run from an "x64 Native Tools Command Prompt for VS".
if not exist build mkdir build
cl /nologo /O2 /EHsc /MT /DUNICODE /D_UNICODE /Fo:build\ /Fe:build\AudioSight.exe src\AudioSight.cpp /link /SUBSYSTEM:WINDOWS gdiplus.lib gdi32.lib user32.lib ole32.lib shell32.lib
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
echo Built build\AudioSight.exe
