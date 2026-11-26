#!/bin/sh
# Build the Linux (X11) version: ./snowdesk
# Needs the X11 development headers: libx11-dev libxext-dev libxrandr-dev libxft-dev libxtst-dev
# If zig is available (pip install ziglang), the binary targets glibc 2.28 so it runs on older distributions too.
set -e
cd "$(dirname "$0")"
SRC="src/linux/main_linux.cpp src/sim.cpp"
CFLAGS="-O2 -std=c++17 $(pkg-config --cflags xft)"
LIBS="-lX11 -lXext -lXrandr -lXft -lXtst -lfontconfig"
if python3 -c "import ziglang" 2>/dev/null; then
  python3 -m ziglang c++ -target x86_64-linux-gnu.2.28 $CFLAGS -idirafter /usr/include -L/usr/lib/x86_64-linux-gnu $SRC -o snowdesk $LIBS -s
else
  g++ $CFLAGS -Wall -Wextra $SRC -o snowdesk $LIBS -s
fi
ls -l snowdesk
