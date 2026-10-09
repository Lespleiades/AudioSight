@echo off
rem Build with MinGW-w64 (g++ must be in PATH)
if not exist build mkdir build
g++ -O2 -s -mwindows -static -static-libgcc -static-libstdc++ -o build\AudioSight.exe src\AudioSight.cpp -lgdiplus -lgdi32 -luser32 -lole32 -lshell32 -luuid
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
echo Built build\AudioSight.exe
