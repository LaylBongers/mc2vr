#!/usr/bin/env bash
# S4-2c probe A: build the 32-bit writer and the 64-bit reader.
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-g++ -static -mconsole -O2 -o writer32.exe writer32.cpp -ld3d9
x86_64-w64-mingw32-g++ -static -mconsole -O2 -o reader64.exe reader64.cpp -ld3d11
echo "built writer32.exe reader64.exe"
