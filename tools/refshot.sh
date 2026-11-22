#!/bin/sh
# tools/refshot.sh — the same text in a reference terminal (kitty, alacritty),
# same font and size, captured like tools/realshot.sh, for side-by-side checks.
#
#   tools/refshot.sh kitty|alacritty out.png 'printf ...' [WxH]
set -u
TERM_NAME=$1; OUT=$2; CMD=$3; CROP=${4:-500x160}
FONT=${FONT:-JetBrainsMono Nerd Font}; PX=${PX:-13}; LH=${LH:-1.2}
DIR=$(mktemp -d)
printf '%s; sleep 30\n' "$CMD" > "$DIR/run.sh"
PT=$(python3 -c "print($PX*72/96)")
case "$TERM_NAME" in
    kitty) setsid kitty --class refshot -o "font_family=$FONT" -o "font_size=$PT" \
               -o "modify_font=cell_height $(python3 -c "print(int($LH*100))")%" -o window_padding_width=8 \
               -o cursor_blink_interval=0 sh "$DIR/run.sh" </dev/null >/dev/null 2>&1 & ;;
    alacritty) setsid alacritty --class refshot -o "font.normal.family=\"$FONT\"" -o "font.size=$PT" \
               -o "window.padding={x=8,y=8}" -e sh "$DIR/run.sh" </dev/null >/dev/null 2>&1 & ;;
esac
PID=$!
i=0
while [ $i -lt 50 ]; do
    GEO=$(hyprctl clients -j | python3 -c "import json,sys; c=next((c for c in json.load(sys.stdin) if c['class']=='refshot'),None); print(c and '%d,%d'%(c['at'][0],c['at'][1]) or '')")
    [ -n "$GEO" ] && break
    i=$((i+1)); sleep 0.1
done
sleep 1.0
grim -g "$GEO $CROP" "$OUT"
ADDR=$(hyprctl clients -j | python3 -c "import json,sys; print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='refshot'),''))")
hyprctl eval "hl.dispatch(hl.dsp.window.close({ window = \"address:$ADDR\" }))" >/dev/null 2>&1
sleep 0.3; pkill -f "$DIR/run.sh" 2>/dev/null; kill $PID 2>/dev/null
rm -rf "$DIR"
echo "refshot: $OUT"
