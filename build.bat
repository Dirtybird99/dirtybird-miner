@echo off
REM DIRTYBIRD C Miner — Windows build via MSYS2 MinGW64
REM Run from the source root (the directory containing this script).

set PATH=C:\msys64\mingw64\bin;%PATH%

if not exist build mkdir build
cd build

cmake -G "MinGW Makefiles" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release ..
if %errorlevel% neq 0 (
    echo CMAKE FAILED
    exit /b 1
)

cmake --build . -j2
if %errorlevel% neq 0 (
    echo BUILD FAILED
    exit /b 1
)

echo.
echo === Binary ready: %cd%\dirtybird-c-miner.exe ===
echo.
echo Test run:
echo   dirtybird-c-miner.exe -d 127.0.0.1:10100 -w YOUR_WALLET_ADDRESS -t 20
