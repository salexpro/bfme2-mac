SAGE Unlocked v1.9.6 (github.com/TheeHorse/SAGE-Unlocked, GPL-3), built here from source on 15 Nov 2026:
  dinput8.dll       zig cc -target x86-windows-gnu -shared -O2 -s -DPROXY_DINPUT8 proxy.c -lkernel32
  CnCFpsUnlocker.dll  wine-mono csc /platform:x86 /optimize /r:System.Core.dll /target:library Unlocker.cs
bfme2-fex.sh copies files/ into C:\BFME2 at every launch.

files/CnCFpsUnlocker.dll is the source-fexfix5 build (1.9.6+fexfix5). Changes against upstream 1.9.6:
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
  fexfix5: network catch-up (BFME II path). A machine that falls behind the other in a network game (a hitch, a long
           load) has ticks of the other's data waiting; the game then stops drawing, drops its frame limiter and runs
           ticks back to back until it is level. The timetable kept handing out one tick per tick length instead, so
           the game spun through ~200,000 empty updates a second between them, drew one frame per tick and never
           caught up: 5 fps for minutes on the machine that was behind, until the other one hitched in turn. While
           more than 3 ticks are waiting (the engine's own threshold at 0x63239d: TheNetwork [0xde4468], vtbl+0x58)
           the stub now leaves the original code alone; with 2 or 3 waiting the timetable runs at double speed until at
           most one is left (otherwise the rest closed at a tick per 20 s). skip=catchup switches it off.
  Checks: SAGEUnlocked.log must say 1.9.6+fexfix5; bfme2/tickrate.sh in a skirmish = about 5 ticks/s; income with one
          fortress = +250 per minute.
Older builds are kept as CnCFpsUnlocker.dll.fexfix1.bak (3x too fast, do not use), .fexfix2.bak and .fexfix3.bak.
A/B switch: an empty file sage-unlocked/disabled makes bfme2-fex.sh remove SAGE Unlocked from the game folder (stock 30 FPS).
