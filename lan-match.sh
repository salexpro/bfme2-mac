#!/bin/bash
# Test automation: start a 2-player LAN match, this Mac hosting and another Mac (set up the same way) joining.
#   lan-match.sh <user@host>        e.g. lan-match.sh user@other-mac.local
. "$(dirname "$0")/env.sh"
DEST="${1:?user@host}"
# The other Mac has this folder in the same place under its home (or name it in BFME_REMOTE_TOOLS) with its own local.conf.
RT="${BFME_REMOTE_TOOLS:-\$HOME${T#"$HOME"}}"
other() { ssh -o BatchMode=yes "$DEST" "cd \"$RT\" && $1"; }
at() { echo "$(( $1 * $3 / 10000 )) $(( $2 * $4 / 10000 ))"; }     # at W H fx fy
other './lan-lobby.sh' > /tmp/bfme2-lan-other.txt 2>&1 &
read -r W H <<< "$("$T/lan-lobby.sh" | tail -1)"; wait
read -r OW OH <<< "$(tail -1 /tmp/bfme2-lan-other.txt)"
[ -n "$W" ] && [ -n "$OW" ] || { echo "lobby failed: here '$W $H', there '$OW $OH'"; exit 1; }
"$T/drive.sh" cmd "click $(at $W $H 8333 4574)" "wait 300"; sleep 8                                  # Create Game
other "./drive.sh cmd 'click $(at $OW $OH 2600 2650)' 'wait 60' 'click $(at $OW $OH 8333 3920)' 'wait 200'"; sleep 10   # pick the game, Join
other "./drive.sh cmd 'click $(at $OW $OH 8767 5957)' 'wait 120'"; sleep 5                             # Ready
"$T/drive.sh" cmd "click $(at $W $H 9115 9570)" "wait 200"; sleep "${LOAD_WAIT:-75}"                  # Play Game
"$T/drive.sh" shot lanmatch >/dev/null; other './drive.sh shot lanmatch >/dev/null'
echo "started: here ${W}x${H}, there ${OW}x${OH}"
