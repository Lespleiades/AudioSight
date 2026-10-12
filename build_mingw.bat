@echo off
rem Build with MinGW-w64 (g++ and windres must be in PATH).
rem src\AudioSight.rc + src\AudioSight.manifest make Windows start the program as administrator.
if not exist build mkdir build
pushd src
windres AudioSight.rc -O coff -o ..\build\AudioSight_res.o
set RC_ERR=%errorlevel%
popd
if not "%RC_ERR%"=="0" (
    echo Resource build failed.
    exit /b 1
)
g++ -O2 -s -mwindows -static -static-libgcc -static-libstdc++ -o build\AudioSight.exe src\AudioSight.cpp build\AudioSight_res.o -lgdiplus -lgdi32 -luser32 -lole32 -lshell32 -luuid -lwinhttp
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
echo Built build\AudioSight.exe
