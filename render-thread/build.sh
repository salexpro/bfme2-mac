#!/bin/bash
# Build render-thread/renderthread.dll (32-bit Windows DLL): the render thread of BFME2 Accelerator, carved out to run
# next to the letterbox proxy under Wine. Needs uv (for Zig's C++ compiler) and git. The accelerator's source is not
# kept here: it is fetched at the commit the carve-out was written against, and carve.py makes the port's copies.
set -e
cd "$(dirname "$0")"
REV=c993d9a
SRC="${BFME_ACCEL_SRC:-.upstream}"
if [ ! -d "$SRC/src" ]; then
  git clone -q https://github.com/OhadBaehr/bfme2-accelerator "$SRC"
  git -C "$SRC" checkout -q "$REV"
fi
mkdir -p .gen
/usr/bin/python3 carve.py "$SRC/src" .gen
uv run -q --with ziglang python -I -m ziglang c++ -target x86-windows-gnu -shared -O2 -std=c++17 -fms-extensions -fno-exceptions \
  -fno-rtti -Wno-everything -I .gen -I . -o renderthread.dll rt_port.cpp -luser32 -lkernel32
rm -f renderthread.pdb renderthread.lib
ls -la renderthread.dll
