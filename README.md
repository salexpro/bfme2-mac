# The Battle for Middle-earth II on Apple Silicon Macs

Tools that make *The Lord of the Rings: The Battle for Middle-earth II* (2006, Patch 1.09 from the community
All in One Launcher) run properly on an Apple Silicon Mac through CrossOver / NotProton: a wide picture without
stretching or display mode switches, a high frame rate at the correct game speed, and no stutter.

No game files are included. You need your own installation of the game.

## What it does

| Problem on a Mac | What happens here |
|---|---|
| The game switches the display mode for its resolution (stretched picture; on some Mac + monitor combinations a mode switch can even crash the machine) | `letterbox/d3d9.dll` creates the real Direct3D device at the current desktop mode and fits the game's picture inside it with black bars. The display mode is never changed. |
| Resolution has to be picked by hand per screen | The launch wrapper measures the main display at every start and sets the resolution (the display's own shape between 4:3 and 16:9, 16:9 on anything wider), the frame-rate cap and (on MacBooks) the notch offset. Plug in another monitor and the next start follows it. |
| The patch's HUD is right only at exactly 16:9; on a MacBook's 16:10 the minimap is an oval, so you play 16:9 with bars | `widescreen-art/hud.py` computes the HUD files for any shape between 4:3 and 16:9 from the game's original and the patch's 16:9 copies. A MacBook uses its whole screen below the camera housing, with a round minimap. |
| Units are invisible (health bars and rings without models) | The game's prefix needs Microsoft's `d3dx9_27.dll`; `setup/setup-prefix.sh` puts it in place. |
| Scrolling and minimap jumps freeze for up to seconds | The terrain's mip-maps are built by a fast filter instead of D3DX (32 ms → 0.13 ms per tile); a camera jump bakes its tiles over a few frames. |
| Locked to 30 fps | A patched build of [SAGE Unlocked](https://github.com/TheeHorse/SAGE-Unlocked) (`sage-unlocked/`) runs the game at the display's refresh rate with game logic at its normal 5 ticks per second. |
| With the unlock under the FEX emulator the game ran 3x too fast | Fixed in the patched SAGE Unlocked (its scheduler kept data on its own code page). |
| With the unlock, fades, animations and the selection ring stepped or ran at the wrong speed | Corrections in `letterbox/d3d9.dll`: the per-tick latch fires once per tick, the animation clock follows the real frame time, flash and ring timings are scaled. |
| With the unlock, smoke, fire, spell effects and the bats over a goblin fortress run too fast, and faster the higher the frame rate (four times too fast at 120 fps) | The game moves its particle effects on by one fixed step per drawn frame. `pfxstep` in `letterbox/d3d9.dll` lets a step through 30 times a second, as at the original 30 fps (the effects then move in 30 steps a second, as they always did). |
| Uneven frame pacing (micro-stutter on steadily moving things) | While the Mac holds the refresh rate, frames are locked to the real buffer swap and the game gets a regular per-frame clock. |
| 16:9 trims the top and bottom of what 4:3 shows | The camera's field of view is widened so the 4:3 height is kept and the sides are added (`horplus`). |
| Round command buttons over buildings are ovals at 16:9 | Drawn round. |
| In a network game a machine that falls behind the other (a hitch, a long load) crawls at 5–15 fps for minutes and cannot catch up | Fixed in the patched SAGE Unlocked: while several ticks of the other side's data are waiting, the game's own catch-up runs (ticks back to back) instead of the unlocker's timetable; with 2–3 waiting the timetable runs at double speed. Level again within seconds. |
| The game freezes for good in a long or big match (one thread at 100 %, no more frames) | A write of the game into a part of its own heap that is not committed is a crash on Windows; the FEX emulator turns it into an endless fault loop. `heapkeep` in `letterbox/d3d9.dll` keeps the game's reserved heap ranges accessible, so the write lands in unused memory. (What makes the game write there: see "Big matches" below.) |
| Crash in the middle of a match, in a network game on both machines at the same moment (access violation at `game.dat` 0x404A21) | A bug of this proxy up to release v1.1, not of the game: `game.dat` was compiled as one unit and keeps values in registers across its own calls, and three of the proxy's hooks were plain C functions that changed them (the tint-flash hook returned with a number in `edx` that the game then used as a pointer). Every hook inside the game's code is now an assembly routine that keeps all registers, the flags and the FPU/SSE state. |
| Low frame rate in battles although the GPU is idle | Measured with the proxy's own profilers: Wine's Direct3D 9 runs on macOS OpenGL with a helper thread, every vertex-buffer write of the game is a synchronous round trip to that thread, and every OpenGL call is an expensive emulator crossing. `noglcheck` stops the error query after each OpenGL call (+12 %), `spinwait` (keeps the helper thread polling instead of sleeping between the round trips) gave another 12 % in a short test but froze the game in a longer one; it is off and should stay off. |
| Battles still fall to 12–20 fps on an M1 Max and to 20–35 on an M5: the game's thread spends most of a frame waiting for Wine's Direct3D | Optional and experimental (off by default): a render thread (`renderthread=1`, see [Render thread](#render-thread-experimental-off-by-default)) records the game's Direct3D calls and replays them on a second thread. In the same saved battle the mean went from 29–30 to 52 fps on an M1 Max and from 53 to 86 on an M5. |
| In a network game the machine that waits for the other one drops to about 70 % of its frame-rate cap (43 fps at a 60 cap) | The game's network throttle, which at 30 fps slowed the logic, is taken off the frame limiter (`netfps`); the logic waits for the network by itself. |
| Main-menu logo and the start-up loading screen are stretched | `widescreen-art/make-art.py` builds pre-squeezed copies from your own game files. |
| The title screen's spinning ring is a coarse oval in an oval hole | The ring is a small video drawn under the title picture and seen through a hole in it. `make-art.py` squeezes the hole's mask (`titlescreenuserinterface.png`) together with the picture, the proxy draws the ring's quad narrower (`ringround`) and has the game play the larger of its two copies of the video, 128x128 instead of 64x64 (`ringhd`). |
| The two start-up loading pictures are 4:3 | Optional: `widescreen-art/loading-screen/` rebuilds both as real 16:9 pictures from a still of the film shot they were made from (you supply the still), carries the pictures' own shading into the new sides, continues the ornament band of the second one (the socket of the spinning ring stays where the game draws the ring) and sharpens them with Real-ESRGAN. |

Multiplayer on a LAN between two Macs set up this way works (lobby, join, matches stay in sync). Each machine needs
its own serial (`ergc` registry value); the setup script takes it as an argument.

## Tested setup

- Mac Studio (M5) and MacBook Pro (M1 Max), macOS 27
- NotProton with its FEX runner (CrossOver 27 preview, build 41073), game started from Steam as a non-Steam shortcut
- BFME II installed with the All in One Launcher, Patch 1.09 v3.1.4 (`game.dat` of game version 1.06)

Every change inside `game.dat` is checked against the expected bytes first and skipped if they differ, so another
game version loses those corrections instead of crashing. Plain CrossOver without NotProton has not been tried.

## Installing

1. Install the game with the All in One Launcher in any CrossOver bottle or NotProton prefix and let it install
   Patch 1.09 and the DirectX 9 runtime.
2. Get this folder (`git clone`, or the source zip of a release) and put the prebuilt DLLs into it: unzip
   `bfme2-mac-binaries.zip` from the [latest release](https://github.com/salexpro/bfme2-mac/releases/latest) inside
   the folder. (Or build them: `letterbox/build.sh` and `sage-unlocked/build.sh`, which need
   [uv](https://docs.astral.sh/uv/).)
3. Add `lotrbfme2.exe` to Steam as a non-Steam game, force the NotProton FEX tool for it, and set the launch options
   to `"<path to this folder>/bfme2-fex.sh" %command%`.
4. Start it once. The wrapper refuses to start the game in a prefix that is not prepared and shows Wine's About box
   instead; close it.
5. Run `setup/setup-prefix.sh`. It finds the prefix of that launch, the game's serial and Microsoft's `d3dx9_27.dll`
   (both in the prefix the launcher installed into) by itself; `--check` only shows what it would use.
6. Start the game from Steam.

The art and HUD for your screen's shape are built from your own game files at the first start when
[uv](https://docs.astral.sh/uv/) is installed (`widescreen-art/out/<shape>/`; by hand:
`uv run --with pillow python widescreen-art/make-art.py <game folder> widescreen-art/out/16x9`, or
`... widescreen-art/out/a1594 1728/1084` for another shape). Without a set for a shape other than 16:9 the wrapper
uses a 16:9 picture. An empty file `widescreen-art/disabled` goes without the art.

Two Macs that are to play each other on a LAN need different serials: give the second one its own
(`setup/setup-prefix.sh auto <serial>`). `setup/copy-to-another-mac.sh` copies the game and this folder to a second
Mac over SSH and does steps 3-5 there.

## Layout

```
bfme2-fex.sh            launch wrapper (Steam launch options:  "<path>/bfme2-fex.sh" %command% )
options-template.ini    first Options.ini for a new prefix
letterbox/              the d3d9.dll proxy: letterbox.c, letterbox.ini (all settings, commented), build.sh
sage-unlocked/          SAGE Unlocked v1.9.6 source, the patched Unlocker.cs, bfme2-fex.patch, build.sh
render-thread/          optional render thread (carved out of BFME2 Accelerator): build.sh, carve.py, rt_port.cpp
setup/                  setup-prefix.sh + prefix-setup.reg (prepare the prefix), copy-to-another-mac.sh
widescreen-art/         make-art.py (logo, loading screen and HUD overrides per picture shape, from your own files),
                        hud.py (the HUD for any shape), loading-screen/ (the 16:9 loading pictures)
env.sh, *.sh in the root   test aids that drive the game through the proxy (see below)
```

Paths that differ per machine are not in the scripts. The launch wrapper takes the game folder from Steam's launch
command; the test aids and `copy-to-another-mac.sh` read `local.conf` in this folder (not tracked, see `env.sh`):

```
BFME_GAME="<folder with lotrbfme2.exe>"
BFME_COMPAT="<Steam library>/steamapps/compatdata/<appid of the shortcut>"
BFME_URL="steam://rungameid/<game id of the shortcut>"
```

## Settings

- `letterbox/letterbox.ini`: every switch of the proxy, each with a comment. `Ctrl+Shift+F` in the game toggles a
  frame-rate counter.
- `display-override.conf` next to the wrapper pins values the wrapper would otherwise measure:
  `RES="1920 1080"`, `FPS=60`, `INSET=0`, `VALIGN=top`, and `LETTERBOX_EXTRA="key=value ..."` for settings of the proxy
  that are to differ from `letterbox.ini` (e.g. `LETTERBOX_EXTRA="renderthread=1"`).
- An empty file `sage-unlocked/disabled` starts the game without the frame-rate unlock (stock 30 fps).

## Big matches

In a big match the game could crash (access violation at `game.dat` 0xB6DD43) or freeze for good. The cause is in the
game: when a unit is knocked into the air, the launch routine (`0x792dbd`) lands it at position + speed x N, where N,
the flight time in logic frames, is `2 x up-speed / (gravity x factor)`. The factor belongs to the unit's knock-back
module, and for the Goblin Cave Troll it is 0. The division gives infinity, N comes out as 4294967296, and the troll
lands billions of units outside the map. The collision manager then files it under a wild index and later crashes on
it; with that prevented, another part of the logic loops for ever on it.

`farguard` (on by default) in `letterbox/d3d9.dll` stops it at the source: the function the game calls on every change
of a unit's position gets a look at the new place first, and a unit that was on the map and would now be beyond
+-100000 is put back where it was. As a second line, a unit that far away is taken out of the collision manager
before its update. `letterbox.log` says which unit it was and which code asked for the move ("far object: ...").
A saved match that crashed about three minutes after every load runs on with it; both machines of a network game
must use the same proxy.

When the game itself faults, the proxy writes a note to `letterbox.log` first ("game fault: ..."): registers, stack
and return addresses. `soak.sh` lets a match run and reports a crash or freeze.

`sage-unlocked/files/SAGEUnlocked.ini` still ships with `skip=eveninterp` (the drawn interpolation that runs evenly
over a logic tick, so steady movers do not hitch five times a second). It was switched off while this crash was
blamed on it; the stress tests that pointed at it most likely just differed in whether a cave troll got knocked
back. It stays off until it has been through long matches again.

## Render thread (experimental, off by default)

Wine's Direct3D 9 takes one global lock in every call and, on macOS OpenGL, makes every buffer lock a round trip to
its own helper thread; the game's thread spends most of a battle frame waiting for it. `render-thread/` carves the
render thread out of [BFME2 Accelerator](https://github.com/OhadBaehr/bfme2-accelerator) by Ohad Baehr (MIT, commit
c993d9a): every Direct3D 9 and D3DX effect call of the game is recorded into a ring and replayed on a second thread,
and the game's thread goes on with the next frame. On top of the accelerator's code the port answers description,
buffer and surface queries from memory (they would otherwise wait for the replay), joins queued buffer writes into one
lock, keeps the animated mouse pointer's small surface in memory, and replaces the accelerator's Windows exception
handling with a guard that works in a 32-bit DLL built with Zig's compiler.

To use it: `render-thread/renderthread.dll` (in the release zip, or built with `render-thread/build.sh`, which
fetches the accelerator's source) and `renderthread=1` in `letterbox/letterbox.ini`, or `LETTERBOX_EXTRA="renderthread=1"`
in `display-override.conf`. The wrapper copies the DLL into the game folder; `letterbox.log` then says "render thread:
renderthread.dll loaded" and `renderthread.log` in the game folder "RENDER THREAD LIVE". It needs the emulator's
default memory ordering: do not combine it with `FEX_TSOENABLED=0`.

The same saved battle, 10 minutes, measured in 10-second windows, without and with it:

| | mean fps | worst minute | lowest 10 s |
|---|---|---|---|
| MacBook Pro M1 Max, 1728x1084 | 29–30 → 52 | 18–19 → 37 | 12 → 25 |
| Mac Studio M5, 2880x1620 | 53 → 86 | 26 → 57 | 20 → 53 |

Tested: two 8-player skirmishes against the AI played to their end (30 and 33 minutes), the saved battle several
times on both Macs, and a 15-minute LAN match with it on both (in sync: the same money on both screens). It changes
how frames are drawn, not what the game computes. Not tested yet: a played network battle, long sessions on the M5,
switching away from the game during a match. An empty file `renderthread.monitor` in the game folder makes
`renderthread.log` report every few seconds what the game's thread waited for.

## Test aids

The proxy reads a command file (`letterbox.cmd` in the game folder): screenshots of the game's own frame, mouse
moves and clicks, a per-frame recorder (brightness, frame time), memory peeks. The scripts in the root use it:
`enter-skirmish.sh`, `scroll-test.sh`, `tickrate.sh`, `interp-trace.sh`, `menu-rec.sh`, `select-rec.sh`,
`ring-rec.sh`, `bring-rec.sh`, `lan-lobby.sh`, `lan-match.sh`, `bench.sh` (load a saved skirmish and measure the
frame rate), `soak.sh` (let a save or a fresh skirmish run and report a crash or freeze; it stops when the match is over), `renderer.sh`. They need no screen-recording permission.

## Licence

GPL-3.0, see `LICENSE`. `sage-unlocked/` is derived from SAGE Unlocked (GPL-3.0). `render-thread/` builds on
BFME2 Accelerator (MIT, `render-thread/LICENSE.accelerator`): `build.sh` fetches its source, `carve.py` copies the
parts used, and `rt_port.cpp` contains code taken from it next to the port's own. The rest is original work under
GPL-3.0. No game files, no artwork and no Microsoft files are included.

## Credits

- [SAGE Unlocked](https://github.com/TheeHorse/SAGE-Unlocked) by TheeHorse, GPL-3.0. `sage-unlocked/` contains its
  v1.9.6 source and a modified copy; the changes are in `sage-unlocked/bfme2-fex.patch`.
- [BFME2 Accelerator](https://github.com/OhadBaehr/bfme2-accelerator) by Ohad Baehr, MIT: the render thread in
  `render-thread/`.
- The BFME community: All in One Launcher and Patch 1.09.
- NotProton, CrossOver / Wine, FEX-Emu.

Not affiliated with Electronic Arts. The Battle for Middle-earth is a trademark of its owners.
