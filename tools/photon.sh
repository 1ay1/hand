#!/bin/sh
# tools/photon.sh — input to photon on the real screen, per interaction.
#
#   tools/photon.sh [scenario] [inputs] [ENV=value ...]
#
# Starts hand in a real window, drives one kind of interaction, and prints
# mayag's measurement: from the kernel's timestamp on each input event to
# the compositor's scanout timestamp of the first frame that showed its
# answer (wp_presentation), with where the time went (to us / in us /
# compositor). Inputs are ~3 refreshes apart so each sample is one input's.
#
#   type      keys echoed by `cat` through the pty                (wtype)
#   settings  ctrl+, opening and closing the settings panel       (wtype)
#   zoom      ctrl+= / ctrl+- : every glyph re-laid, ptys resized  (wtype)
#   pane      ctrl+shift+e opening and closing the file pane      (wtype)
#   tabs      ctrl+tab across four tabs                           (wtype)
#   drag      dragging the file pane's edge                       (virtual mouse)
#   select    drag-selecting text                                 (virtual mouse)
#   scroll    the wheel in the scrollback                         (virtual mouse)
#   all       every one of them
#
# Env passes through to hand (MAYAG_PHOTON=2: every frame's stages;
# MAYAG_VSYNC=0). PHOTON_VIBE appends hand.vibe settings. HAND=./build-rel/hand
# measures the release build (the number that matters).
set -u
case "${1:-}" in ''|*[!a-z]*) SCEN=type ;; *) SCEN=$1; shift ;; esac
case "${1:-}" in ''|*[!0-9]*) N=100 ;; *) N=$1; shift ;; esac
HAND=${HAND:-./build-rel/hand}
[ -x "$HAND" ] || HAND=./build/hand
TOOLS=$(dirname "$0")

if [ "$SCEN" = all ]; then
    for s in type settings zoom pane tabs drag select scroll; do
        printf '%-9s ' "$s"; "$0" "$s" "$N" "$@" | tr '\n' ' ' | sed 's/mayag: input->photon  //; s/  stages (mean)://'; echo
    done
    exit 0
fi

VMOUSE=/tmp/hand-vmouse
if [ ! -x "$VMOUSE" ] || [ "$TOOLS/vmouse.c" -nt "$VMOUSE" ]; then
    cc -O2 -o "$VMOUSE" "$TOOLS/vmouse.c" || { echo "photon: cannot build vmouse"; exit 1; }
fi
SCREEN=$(hyprctl monitors -j | python3 -c "import json,sys; m=json.load(sys.stdin)[0]; print(m['width'], m['height'])")

DIR=$(mktemp -d)
printf "confirm {\n  close never\n}\nfiles {\n  side left\n  width 300\n  watch false\n}\ntabs {\n  always true\n}\n${PHOTON_VIBE:-}" > "$DIR/hand.vibe"
PROG=cat
case "$SCEN" in scroll|select) PROG="seq 1 20000; exec cat" ;; esac
env MAYAG_PHOTON=1 HAND_CONFIG="$DIR/hand.vibe" "$@" setsid "$HAND" -e "$PROG" </dev/null >"$DIR/out.log" 2>&1 &
PID=$!
i=0
while [ $i -lt 50 ]; do   # up to 5 s for the window
    GEO=$(hyprctl clients -j | python3 -c "import json,sys; c=next((c for c in json.load(sys.stdin) if c['pid']==$PID),None); print(c and '%s %d %d %d %d'%(c['address'],c['at'][0],c['at'][1],c['size'][0],c['size'][1]) or '')")
    [ -n "$GEO" ] && break
    i=$((i+1)); sleep 0.1
done
[ -z "$GEO" ] && { echo "photon: no window"; kill $PID; exit 1; }
set -- $GEO
ADDR=$1; WX=$2; WY=$3; WW=$4; WH=$5
hyprctl dispatch focuswindow "address:$ADDR" >/dev/null
sleep 0.6
FOCUS=$(hyprctl activewindow -j | python3 -c "import json,sys; print(json.load(sys.stdin).get('pid',0))")
if [ "$FOCUS" != "$PID" ]; then echo "photon: hand did not get the focus; not driving it"; kill $PID; exit 1; fi

key()   { wtype "$@"; sleep 0.03; }
mouse() { "$VMOUSE" $SCREEN; }
TABS=1
k=0
case "$SCEN" in
    type)     while [ $k -lt "$N" ]; do key -d 1 a; k=$((k+1)); done ;;
    settings) while [ $k -lt "$N" ]; do key -M ctrl -k comma -m ctrl; k=$((k+1)); done
              [ $((N % 2)) = 1 ] && key -M ctrl -k comma -m ctrl ;;
    zoom)     while [ $k -lt "$N" ]; do
                  if [ $((k % 2)) = 0 ]; then key -M ctrl -k equal -m ctrl; else key -M ctrl -k minus -m ctrl; fi
                  k=$((k+1))
              done ;;
    pane)     while [ $k -lt "$N" ]; do key -M ctrl -M shift -k e -m shift -m ctrl; k=$((k+1)); done
              [ $((N % 2)) = 1 ] && key -M ctrl -M shift -k e -m shift -m ctrl ;;
    tabs)     key -M ctrl -M shift -k t -m shift -m ctrl; key -M ctrl -M shift -k t -m shift -m ctrl
              key -M ctrl -M shift -k t -m shift -m ctrl; TABS=4; sleep 0.5
              while [ $k -lt "$N" ]; do key -M ctrl -k Tab -m ctrl; k=$((k+1)); done ;;
    drag)     key -M ctrl -M shift -k e -m shift -m ctrl; sleep 0.5
              GX=$((WX + 300 - 2)); GY=$((WY + WH / 2))
              { echo "m $GX $GY"; echo "s 80"; echo d; echo "s 40"
                while [ $k -lt "$N" ]; do echo "m $((GX + (k % 20) * 10)) $GY"; echo "s 30"; k=$((k+1)); done
                echo u; echo "s 40"; } | mouse ;;
    select)   SX=$((WX + 40)); SY=$((WY + WH / 3))
              { echo "m $SX $SY"; echo "s 80"; echo d; echo "s 40"
                while [ $k -lt "$N" ]; do echo "m $((SX + (k % 30) * 12)) $((SY + (k % 7) * 18))"; echo "s 30"; k=$((k+1)); done
                echo u; echo "s 40"; } | mouse ;;
    scroll)   { echo "m $((WX + WW / 2)) $((WY + WH / 2))"; echo "s 80"
                while [ $k -lt "$N" ]; do
                    if [ $k -lt $((N / 2)) ]; then echo "w -1"; else echo "w 1"; fi; echo "s 30"; k=$((k+1))
                done; } | mouse ;;
    *) echo "photon: no scenario '$SCEN'"; kill $PID; exit 1 ;;
esac
sleep 0.4
t=0   # end it from inside: each tab's cat ends, the last tab closes, hand quits
while [ $t -lt $TABS ]; do wtype -k Escape; wtype -k Return; sleep 0.05; wtype -M ctrl d -m ctrl; sleep 0.15; t=$((t+1)); done
i=0; while kill -0 $PID 2>/dev/null && [ $i -lt 50 ]; do i=$((i+1)); sleep 0.1; done
if kill -0 $PID 2>/dev/null; then echo "photon: hand did not exit"; kill -9 $PID; fi
grep -A1 "input->photon" "$DIR/out.log" || { echo "photon: no measurement"; tail -5 "$DIR/out.log"; }
[ -n "${KEEP:-}" ] && echo "log: $DIR/out.log" || rm -rf "$DIR"
