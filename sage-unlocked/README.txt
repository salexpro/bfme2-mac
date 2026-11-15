SAGE Unlocked v1.9.6 (github.com/TheeHorse/SAGE-Unlocked, GPL-3), built here from source on 15 Nov 2026:
  dinput8.dll       zig cc -target x86-windows-gnu -shared -O2 -s -DPROXY_DINPUT8 proxy.c -lkernel32
  CnCFpsUnlocker.dll  wine-mono csc /platform:x86 /optimize /r:System.Core.dll /target:library Unlocker.cs
bfme2-fex.sh copies files/ into C:\BFME2 at every launch.

files/CnCFpsUnlocker.dll is the source-fexfix4 build (1.9.6+fexfix4). Changes against upstream 1.9.6:
  fexfix1: scroll/spellstore scratch data on a separate read-write page (BFME II path).
  fexfix2: the scheduler's per-frame variables (clock, tick start, phase tables) on a separate read-write page, stubs stay on
           the code page (BFME II path). REQUIRED under the FEX emulator: with data and code on one page the stub's "ms*3"
           ran twice (clock = timeGetTime*9), so game logic ran 15 ticks/s instead of 5 = everything 3x too fast.
  fexfix3: multiplayer smoothing. In a network game the network decides when a tick starts; the schedule lagged behind it,
           so all six phases ran in the first frames of a tick and the drawn interpolation reached 1.0 after ~70 ms of the
           200 ms tick (everything moved, then stood still: "missing frames"). When a tick that the network held back finally
           starts, the schedule is restarted from that frame. Single-player timing is unchanged.
  fexfix4: even smoothing (BFME II path). The game latches values at the first frame of a tick (phase 1) and draws
           base + interp * delta. interp was measured from a sixth of a tick before that frame: it began each tick at 1/6,
           reached 1.0 a sixth early and froze for 3-4 frames at 120 fps, then jumped (steady movers such as the crows
           showed missing frames five times a second). It now runs frame/tick .. 1.0 from the tick's first frame.
           bfme2/interp-trace.sh prints phase:interp per frame.
  Checks: SAGEUnlocked.log must say 1.9.6+fexfix4; bfme2/tickrate.sh in a skirmish = about 5 ticks/s; income with one
          fortress = +250 per minute.
Older builds are kept as CnCFpsUnlocker.dll.fexfix1.bak (3x too fast, do not use), .fexfix2.bak and .fexfix3.bak.
A/B switch: an empty file sage-unlocked/disabled makes bfme2-fex.sh remove SAGE Unlocked from the game folder (stock 30 FPS).
