# The Battle for Middle-earth II on Apple Silicon Macs

Tools that make *The Lord of the Rings: The Battle for Middle-earth II* (2006, Patch 1.09 from the community
All in One Launcher) run properly on an Apple Silicon Mac through CrossOver / NotProton: a wide picture without
stretching or display mode switches, a high frame rate at the correct game speed, and no stutter.

No game files are included. You need your own installation of the game.

## What it does

| Problem on a Mac | What happens here |
|---|---|
| The game switches the display mode for its resolution (stretched picture; on some Mac + monitor combinations a mode switch can even crash the machine) | `letterbox/d3d9.dll` creates the real Direct3D device at the current desktop mode and fits the game's picture inside it with black bars. The display mode is never changed. |
| Resolution has to be picked by hand per screen | The launch wrapper measures the main display at every start and sets a 16:9 resolution, the frame-rate cap and (on MacBooks) the notch offset. |
| Units are invisible (health bars and rings without models) | The game's prefix needs Microsoft's `d3dx9_27.dll`; `transfer/setup-prefix.sh` puts it in place. |
| Scrolling and minimap jumps freeze for up to seconds | The terrain's mip-maps are built by a fast filter instead of D3DX (32 ms → 0.13 ms per tile); a camera jump bakes its tiles over a few frames. |
| Locked to 30 fps | A patched build of [SAGE Unlocked](https://github.com/TheeHorse/SAGE-Unlocked) (`sage-unlocked/`) runs the game at the display's refresh rate with game logic at its normal 5 ticks per second. |
| With the unlock under the FEX emulator the game ran 3x too fast | Fixed in the patched SAGE Unlocked (its scheduler kept data on its own code page). |
| With the unlock, fades, animations and the selection ring stepped or ran at the wrong speed | Corrections in `letterbox/d3d9.dll`: the per-tick latch fires once per tick, the animation clock follows the real frame time, flash and ring timings are scaled. |
| Uneven frame pacing (micro-stutter on steadily moving things) | While the Mac holds the refresh rate, frames are locked to the real buffer swap and the game gets a regular per-frame clock. |
| 16:9 trims the top and bottom of what 4:3 shows | The camera's field of view is widened so the 4:3 height is kept and the sides are added (`horplus`). |
| Round command buttons over buildings are ovals at 16:9 | Drawn round. |
| In a network game the machine that waits for the other one drops to about 70 % of its frame-rate cap (43 fps at a 60 cap) | The game's network throttle, which at 30 fps slowed the logic, is taken off the frame limiter (`netfps`); the logic waits for the network by itself. |
| Main-menu logo and the start-up loading screen are stretched | `widescreen-art/make-art.py` builds pre-squeezed copies from your own game files. |
| The loading screen is a 4:3 picture | Optional: `widescreen-art/loading-screen/` rebuilds it as a real 16:9 picture from a still of the film shot it was made from (you supply the still), carries the picture's own shading into the new sides and sharpens it with Real-ESRGAN. |

Multiplayer on a LAN between two Macs set up this way works (lobby, join, matches stay in sync). Each machine needs
its own serial (`ergc` registry value); the setup script takes it as an argument.

## Tested setup

- Mac Studio (M5) and MacBook Pro (M1 Max), macOS 27
- NotProton with its FEX runner (CrossOver 27 preview, build 41073), game started from Steam as a non-Steam shortcut
- BFME II installed with the All in One Launcher, Patch 1.09 v3.1.4 (`game.dat` of game version 1.06)

Every change inside `game.dat` is checked against the expected bytes first and skipped if they differ, so another
game version loses those corrections instead of crashing. Plain CrossOver without NotProton has not been tried.

## Layout

```
bfme2-fex.sh            launch wrapper (Steam launch options:  "<path>/bfme2-fex.sh" %command% )
options-template.ini    first Options.ini for a new prefix
letterbox/              the d3d9.dll proxy: letterbox.c, letterbox.ini (all settings, commented), build.sh
sage-unlocked/          SAGE Unlocked v1.9.6 source, the patched Unlocker.cs, bfme2-fex.patch, build.sh
transfer/               prefix-setup.reg, setup-prefix.sh (prepare a prefix), push-to-mac.sh (copy to another Mac)
widescreen-art/         make-art.py (16:9 logo / loading screen overrides from your own files), loading-screen/ (see above)
env.sh, *.sh in the root   test aids that drive the game through the proxy (see below)
```

## Installing

1. Install the game with the All in One Launcher in any CrossOver bottle or NotProton prefix and let it install
   Patch 1.09 and the DirectX 9 runtime.
2. Build the binaries: `letterbox/build.sh` and `sage-unlocked/build.sh` (need [uv](https://docs.astral.sh/uv/);
   the C# part is compiled with the Wine Mono that ships in the runner).
3. Copy Microsoft's 32-bit `d3dx9_27.dll` (from the launcher's bottle: `windows/syswow64/d3dx9_27.dll`) to
   `transfer/d3dx9_27.dll`. It is not included here.
4. Add `lotrbfme2.exe` to Steam as a non-Steam game, force the NotProton FEX tool, and set the launch options to
   `"<path to this folder>/bfme2-fex.sh" %command%`.
5. Start it once. The wrapper refuses to start the game until the prefix is prepared and shows Wine's About box
   instead; close it.
6. Run `transfer/setup-prefix.sh <shortcut appid> <serial> [game folder]`. It imports `prefix-setup.reg`, installs
   `d3dx9_27.dll` and links `C:\BFME2` to the game folder.
7. Optional: `uv run --with pillow python widescreen-art/make-art.py <game folder> widescreen-art/out`.
8. Start the game from Steam.

`transfer/push-to-mac.sh` does steps 4-6 on a second Mac over SSH.

Paths that differ per machine are not in the scripts. The launch wrapper takes the game folder from Steam's launch
command; the test aids and `push-to-mac.sh` read `local.conf` in this folder (not tracked, see `env.sh`):

```
BFME_GAME="<folder with lotrbfme2.exe>"
BFME_COMPAT="<Steam library>/steamapps/compatdata/<appid of the shortcut>"
BFME_URL="steam://rungameid/<game id of the shortcut>"
```

## Settings

- `letterbox/letterbox.ini`: every switch of the proxy, each with a comment. `Ctrl+Shift+F` in the game toggles a
  frame-rate counter.
- `display-override.conf` next to the wrapper pins values the wrapper would otherwise measure:
  `RES="1920 1080"`, `FPS=60`, `INSET=0`, `VALIGN=top`.
- An empty file `sage-unlocked/disabled` starts the game without the frame-rate unlock (stock 30 fps).

## Test aids

The proxy reads a command file (`letterbox.cmd` in the game folder): screenshots of the game's own frame, mouse
moves and clicks, a per-frame recorder (brightness, frame time), memory peeks. The scripts in the root use it:
`enter-skirmish.sh`, `scroll-test.sh`, `tickrate.sh`, `interp-trace.sh`, `menu-rec.sh`, `select-rec.sh`,
`ring-rec.sh`, `bring-rec.sh`, `lan-lobby.sh`, `lan-match.sh`. They need no screen-recording permission.

## Licence

GPL-3.0, see `LICENSE`. `sage-unlocked/` is derived from SAGE Unlocked (GPL-3.0); the rest is original work under the
same licence. No game files, no artwork and no Microsoft files are included.

## Credits

- [SAGE Unlocked](https://github.com/TheeHorse/SAGE-Unlocked) by TheeHorse, GPL-3.0. `sage-unlocked/` contains its
  v1.9.6 source and a modified copy; the changes are in `sage-unlocked/bfme2-fex.patch`.
- The BFME community: All in One Launcher and Patch 1.09.
- NotProton, CrossOver / Wine, FEX-Emu.

Not affiliated with Electronic Arts. The Battle for Middle-earth is a trademark of its owners.
