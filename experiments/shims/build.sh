#!/usr/bin/env bash
set -euo pipefail

# Build LeagueClientUxShim.exe with 64-bit MinGW GCC
x86_64-w64-mingw32-gcc -O2 -s -mconsole \
    LeagueClientUxShim.c -o LeagueUxS01.exe -lws2_32

echo "Built LeagueUxS01.exe successfully!"
