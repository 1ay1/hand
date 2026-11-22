#!/bin/sh
# tools/realshot.sh — a frame of hand's REAL window (your config, system
# fonts, the monitor's scale), for checks the headless shots can't make.
#
#   tools/realshot.sh out.png 'printf "\033[7mHHHH\033[0m\n"' [WxH crop]
set -u
OUT=$1; CMD=$2; CROP=${3:-500x160}
HAND=${HAND:-./build-rel/hand}
DIR=$(mktemp -d)
cp "${HAND_CONFIG:-$HOME/.config/hand/hand.vibe}" "$DIR/hand.vibe" 2>/dev/null || : > "$DIR/hand.vibe"
printf '\nconfirm {\n  close never\n}\n' >> "$DIR/hand.vibe"
printf '%s; exec cat\n' "$CMD" > "$DIR/run.sh"
HAND_CONFIG="$DIR/hand.vibe" setsid "$HAND" -e "sh $DIR/run.sh" </dev/null >"$DIR/out.log" 2>&1 &
PID=$!
i=0
while [ $i -lt 50 ]; do
    GEO=$(hyprctl clients -j | python3 -c "import json,sys; c=next((c for c in json.load(sys.stdin) if c['pid']==$PID),None); print(c and '%d,%d'%(c['at'][0],c['at'][1]) or '')")
    [ -n "$GEO" ] && break
    i=$((i+1)); sleep 0.1
done
[ -z "$GEO" ] && { echo "realshot: no window"; kill $PID; exit 1; }
sleep 0.8
if [ -n "${KEYS:-}" ]; then eval "$KEYS"; sleep 0.6; fi
grim -g "$GEO $CROP" "$OUT"
[ -n "${UNDO:-}" ] && eval "$UNDO" && sleep 0.3   # never Escape: a compositor binding with a latched modifier
wtype -k Return; sleep 0.05; wtype -M ctrl d -m ctrl
i=0; while kill -0 $PID 2>/dev/null && [ $i -lt 30 ]; do i=$((i+1)); sleep 0.1; done
kill -9 $PID 2>/dev/null
rm -rf "$DIR"
echo "realshot: $OUT"
