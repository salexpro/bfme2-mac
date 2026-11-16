#!/bin/bash
# Build the patched SAGE Unlocked (GPL-3, github.com/TheeHorse/SAGE-Unlocked) used here:
#   files/dinput8.dll         the unmodified v1.9.6 loader (proxy.c), 32-bit, with Zig's C compiler through uv
#   files/CnCFpsUnlocker.dll  source-fexfix7/Unlocker.cs (v1.9.6 + bfme2-fex.patch), compiled with the C# compiler that
#                             ships in Wine Mono inside a CrossOver / NotProton runner (no .NET SDK needed on the Mac)
# Usage: build.sh [path to the runner's CrossOver folder]   (default: NotProton's FEX runner)
set -e
cd "$(dirname "$0")"
R="${1:-$HOME/Library/Application Support/notproton/runners/crossover-27.0.0.41073-fex/CrossOver}"
uv run -q --with ziglang python -I -m ziglang cc -target x86-windows-gnu -shared -O2 -s -DPROXY_DINPUT8 \
  -o files/dinput8.dll source-v1.9.6/proxy.c -lkernel32
W="$R/lib/wine/aarch64-unix/wine.app/Contents/MacOS/wine"; [ -x "$W" ] || W="$R/lib/wine/x86_64-unix/wine"
[ -x "$W" ] || { echo "no wine in $R"; exit 1; }
export WINEPREFIX="${TMPDIR:-/tmp}/sage-unlocked-build-prefix" WINESERVER="$R/bin/wineserver-arm64" WINELOADER="$W" CX_ROOT="$R" WINEDEBUG=-all
[ -x "$WINESERVER" ] || unset WINESERVER
( cd source-fexfix7 && "$W" 'C:\windows\Microsoft.NET\Framework\v4.0.30319\csc.exe' /nologo /platform:x86 /optimize /r:System.Core.dll \
    /target:library /out:CnCFpsUnlocker.dll Unlocker.cs && mv CnCFpsUnlocker.dll ../files/ )
rm -f files/proxy.lib files/*.pdb
ls -la files
