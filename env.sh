# Shared settings for the test scripts in this folder (sourced by them, not run).
# Machine-specific values come from the environment or from local.conf next to this file (not tracked by git):
#   BFME_GAME="<the folder with lotrbfme2.exe>"
#   BFME_COMPAT="<Steam library>/steamapps/compatdata/<appid of the game's Steam shortcut>"
#   BFME_URL="steam://rungameid/<game id of the shortcut>"       (appid * 2^32 + 0x02000000)
#   BFME_SHOTS="<where screenshots go>"                           (default: shots/ here)
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_g="${BFME_GAME:-}"; _c="${BFME_COMPAT:-}"; _u="${BFME_URL:-}"; _s="${BFME_SHOTS:-}"
[ -f "$T/local.conf" ] && . "$T/local.conf"
BFME_GAME="${_g:-${BFME_GAME:-}}"; BFME_COMPAT="${_c:-${BFME_COMPAT:-}}"; BFME_URL="${_u:-${BFME_URL:-}}"; BFME_SHOTS="${_s:-${BFME_SHOTS:-$T/shots}}"
unset _g _c _u _s
[ -d "$BFME_GAME" ] || { echo "BFME_GAME is not set: put BFME_GAME=\"<folder with lotrbfme2.exe>\" into $T/local.conf" >&2; exit 1; }
export BFME_GAME BFME_COMPAT BFME_URL BFME_SHOTS
G="$BFME_GAME"; CD="$BFME_COMPAT"; URL="$BFME_URL"
need_steam() {   # for the scripts that start the game through Steam
  [ -n "$CD" ] && [ -n "$URL" ] || { echo "BFME_COMPAT and BFME_URL are not set (see env.sh)" >&2; exit 1; }
}
game_up() {     # wait (up to $1 s, default 90) until the game has created its Direct3D device; the proxy logs that
  local i; for i in $(seq 1 "${1:-90}"); do grep -q "^CreateDevice: game" "$G/letterbox.log" 2>/dev/null && return 0; sleep 1; done; return 1
}
game_size() {   # prints "W H": the game's resolution (logged whether the picture is letterboxed or fills the screen)
  tr -d '\r' < "$G/letterbox.log" 2>/dev/null | sed -n 's/^CreateDevice: game \([0-9]*\)x\([0-9]*\) .*/\1 \2/p' | tail -1
}
