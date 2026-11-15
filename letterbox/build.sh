#!/bin/bash
# Build letterbox/d3d9.dll (32-bit Windows DLL) with Zig's C compiler, fetched on demand through uv.
# Needs: uv (https://docs.astral.sh/uv/). Any clang/mingw able to target i686 Windows works too.
set -e
cd "$(dirname "$0")"
uv run -q --with ziglang python -I -m ziglang cc -target x86-windows-gnu -shared -O2 -Wall -Wno-unused-function -Wno-frame-address \
  -o d3d9.dll letterbox.c -luser32 -lkernel32
rm -f d3d9.pdb d3d9.lib letterbox.lib
ls -la d3d9.dll
