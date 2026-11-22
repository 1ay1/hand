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
# its own empty workspace (opened there, so it gets the focus even when a
# fullscreen window holds it elsewhere); back where we were on exit
HOME_WS=$(hyprctl activeworkspace -j | python3 -c "import json,sys; print(json.load(sys.stdin)['id'])")
trap 'hyprctl eval "hl.dispatch(hl.dsp.focus({ workspace = \"$HOME_WS\" }))" >/dev/null 2>&1' EXIT
hyprctl eval 'hl.dispatch(hl.dsp.focus({ workspace = "77" }))' >/dev/null   # the name: a number is ignored
env MAYAG_PHOTON=${PHOTON_TRACE:-2} HAND_CONFIG="$DIR/hand.vibe" "$@" setsid "$HAND" -e "$PROG" </dev/null >"$DIR/out.log" 2>&1 &
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
[ -z "${PHOTON_WINDOWED:-}" ] && hyprctl eval 'hl.dispatch(hl.dsp.window.fullscreen({ mode = "fullscreen" }))' >/dev/null
sleep 0.6
set -- $(hyprctl clients -j | python3 -c "import json,sys; c=next(c for c in json.load(sys.stdin) if c['pid']==$PID); print(c['at'][0],c['at'][1],c['size'][0],c['size'][1])")
WX=$1; WY=$2; WW=$3; WH=$4
sleep 0.3
# keyboard focus follows the mouse here (input:follow_mouse = 1), and the
# compositor refuses focus requests: put the pointer on hand to focus it
printf 'm %d %d\ns 150\n' $((WX + WW / 2)) $((WY + WH / 2)) | "$VMOUSE" $SCREEN
i=0
while :; do
    FOCUS=$(hyprctl clients -j | python3 -c "import json,sys; c=next((c for c in json.load(sys.stdin) if c['focusHistoryID']==0),{}); print(c.get('pid',0))")
    [ "$FOCUS" = "$PID" ] && break
    i=$((i+1)); [ $i -gt 10 ] && break
    hyprctl dispatch focuswindow "address:$ADDR" >/dev/null; sleep 0.2
done
case "$SCEN" in drag|select|scroll) ;;   # pointer input goes to the surface under the pointer: no focus needed
    *) if [ "$FOCUS" != "$PID" ]; then echo "photon: hand did not get the focus; not driving it"; kill $PID; exit 1; fi ;; esac

key()   { wtype "$@"; sleep 0.03; }
# A modified chord. wtype gives the FIRST keysym it sends keycode 9, and the
# compositor matches its binds against the real keymap, where 9 is Escape:
# `wtype -M ctrl -M shift -k e` is ctrl+shift+Escape to Hyprland (a btop
# window per press). So a bare Shift_L goes first, unmodified, to take keycode 9 (plain
# Escape is no bind; hand ignores a lone modifier), and the chord's key gets keycode 10.
chord() { wtype -k Shift_L "$@"; sleep 0.03; }
mouse() { "$VMOUSE" $SCREEN; }
TABS=1
k=0
case "$SCEN" in
    type)     while [ $k -lt "$N" ]; do key -d 1 a; k=$((k+1)); done ;;
    settings) while [ $k -lt "$N" ]; do chord -M ctrl -k comma -m ctrl; k=$((k+1)); done
              [ $((N % 2)) = 1 ] && chord -M ctrl -k comma -m ctrl ;;
    zoom)     while [ $k -lt "$N" ]; do
                  if [ $((k % 2)) = 0 ]; then chord -M ctrl -k equal -m ctrl; else chord -M ctrl -k minus -m ctrl; fi
                  k=$((k+1))
              done ;;
    pane)     while [ $k -lt "$N" ]; do chord -M ctrl -M shift -k e -m shift -m ctrl; k=$((k+1)); done
              [ $((N % 2)) = 1 ] && chord -M ctrl -M shift -k e -m shift -m ctrl ;;
    tabs)     chord -M ctrl -M shift -k t -m shift -m ctrl; chord -M ctrl -M shift -k t -m shift -m ctrl
              chord -M ctrl -M shift -k t -m shift -m ctrl; TABS=4; sleep 0.5
              while [ $k -lt "$N" ]; do chord -M ctrl -k Tab -m ctrl; k=$((k+1)); done ;;
    drag)     chord -M ctrl -M shift -k e -m shift -m ctrl; sleep 0.5
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
# never Escape here: with a modifier still latched it is a compositor binding
# (ctrl+shift+Escape opened 129 btop windows). The drag left the pane open.
while [ $t -lt $TABS ]; do wtype -k Return; sleep 0.05; wtype -k Shift_L -M ctrl d -m ctrl; sleep 0.15; t=$((t+1)); done
i=0; while kill -0 $PID 2>/dev/null && [ $i -lt 50 ]; do i=$((i+1)); sleep 0.1; done
if kill -0 $PID 2>/dev/null; then
    echo "photon: hand did not exit"
    [ -n "${KEEP:-}" ] && grim -g "$WX,$WY ${WW}x${WH}" -s 0.5 "$DIR/stuck.png" && echo "stuck: $DIR/stuck.png"
    kill -9 $PID
fi
# the summary from every frame's trace (needs no clean exit)
python3 - "$DIR/out.log" <<'PY' || echo "photon: no measurement"
import sys
r=[l.split() for l in open(sys.argv[1]) if l.startswith('photon ')]
if not r: sys.exit(1)
# photon T  to-us A  in-us B  compositor C  =  TOTAL ms  (period P)
tot=sorted(float(x[x.index('=')+1]) for x in r); n=len(tot)
st=[sum(float(x[i]) for x in r)/n for i in (3,5,7)]
print("mayag: input->photon  best %.2f  p50 %.2f  mean %.2f  p95 %.2f  worst %.2f ms  (%d frames)" % (tot[0], tot[n//2], sum(tot)/n, tot[min(n-1,int(n*.95))], tot[-1], n))
print("  stages (mean): to us %.2f  in us %.2f  compositor %.2f ms" % tuple(st))
PY
[ -n "${KEEP:-}" ] && echo "log: $DIR/out.log" || rm -rf "$DIR"
