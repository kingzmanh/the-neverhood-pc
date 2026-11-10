#!/bin/sh
# Dev build: ./build.sh [gen] - regenerate the recompiled code (optional) and build.
cd "$(dirname "$0")"
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
if [ "$1" = gen ]; then make gen | grep -v "^unimplemented" || exit 1; fi
make -j${JOBS:-16} 2>&1 | grep -E "error|undefined reference|warning: implicit" | head -40
ls -la *.exe 2>/dev/null | awk '{print $5, $9}'
