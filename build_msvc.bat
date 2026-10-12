@echo off
rem Build with Visual Studio. Run from an "x64 Native Tools Command Prompt for VS".
rem /MANIFESTUAC makes Windows start the program as administrator (no extra file needed).
if not exist build mkdir build
cl /nologo /O2 /EHsc /MT /DUNICODE /D_UNICODE /Fo:build\ /Fe:build\AudioSight.exe src\AudioSight.cpp /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" gdiplus.lib gdi32.lib user32.lib ole32.lib shell32.lib winhttp.lib
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
echo Built build\AudioSight.exe
