#!/bin/sh
# tools/photon.sh — input to photon, measured on the real screen.
#
# Starts hand in a real window running `cat` (the tty echoes each key, so
# every keystroke has an answer on screen), types N keys into it with wtype,
# and prints mayag's measurement: from the kernel's timestamp on each key
# event to the compositor's scanout timestamp of the frame that showed it
# (wp_presentation). Types only once hand's own window has the focus.
#
#   tools/photon.sh [keys=200] [extra env, e.g. MAYAG_PRESENT=fifo]
set -u
N=${1:-200}; shift 2>/dev/null || true
HAND=${HAND:-./build/hand}
DIR=$(mktemp -d)
printf "confirm {\n  close never\n}\n${PHOTON_VIBE:-}" > "$DIR/hand.vibe"
env MAYAG_PHOTON=1 HAND_CONFIG="$DIR/hand.vibe" "$@" setsid "$HAND" -e cat </dev/null >"$DIR/out.log" 2>&1 &
PID=$!
i=0
while [ $i -lt 50 ]; do   # up to 5 s for the window
    ADDR=$(hyprctl clients -j | python3 -c "import json,sys; print(next((c['address'] for c in json.load(sys.stdin) if c['pid']==$PID),''))")
    [ -n "$ADDR" ] && break
    i=$((i+1)); sleep 0.1
done
[ -z "$ADDR" ] && { echo "photon: no window"; kill $PID; exit 1; }
hyprctl dispatch focuswindow "address:$ADDR" >/dev/null
[ -n "${PHOTON_FULLSCREEN:-}" ] && hyprctl eval "hl.dispatch(hl.dsp.window.fullscreen({ mode = \"fullscreen\" }))" >/dev/null
sleep 0.4
FOCUS=$(hyprctl activewindow -j | python3 -c "import json,sys; print(json.load(sys.stdin).get('pid',0))")
if [ "$FOCUS" != "$PID" ]; then echo "photon: hand did not get the focus; not typing"; kill $PID; exit 1; fi
k=0
while [ $k -lt "$N" ]; do   # one key per ~30 ms: each one its own frame
    wtype -d 1 a
    sleep 0.03
    k=$((k+1))
done
sleep 0.3
wtype -k Return; sleep 0.1; wtype -M ctrl d -m ctrl   # cat ends; the last tab closes; hand quits
i=0; while kill -0 $PID 2>/dev/null && [ $i -lt 50 ]; do i=$((i+1)); sleep 0.1; done
if kill -0 $PID 2>/dev/null; then [ -n "${KEEP:-}" ] && grim -g "$(hyprctl clients -j | python3 -c "import json,sys; c=next(c for c in json.load(sys.stdin) if c['pid']==$PID); print('%d,%d %dx%d'%(c['at'][0],c['at'][1],c['size'][0],c['size'][1]))")" -s 0.5 "$DIR/stuck.png"; echo "photon: hand did not exit"; kill -9 $PID; fi
grep -A1 "input->photon" "$DIR/out.log" || { echo "photon: no measurement"; tail -5 "$DIR/out.log"; }
[ -n "${KEEP:-}" ] && echo "log: $DIR/out.log" || rm -rf "$DIR"
