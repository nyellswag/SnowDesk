#!/bin/sh
# Build SnowDesk.exe (cross-compile from Linux with mingw-w64, or run the same commands in MSYS2 on Windows)
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-windres src/app.rc -O coff -o app.res
x86_64-w64-mingw32-g++ -O2 -s -std=c++17 -municode -mwindows -static -static-libgcc -static-libstdc++ \
  -Wall -Wextra -Wno-cast-function-type \
  src/main.cpp src/sim.cpp app.res -o SnowDesk.exe -ldwmapi -lshell32 -luser32 -lgdi32 -ladvapi32
rm -f app.res
ls -l SnowDesk.exe
