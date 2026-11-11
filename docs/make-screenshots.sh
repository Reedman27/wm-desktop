#!/bin/sh
# Regenerates docs/screenshots/*.png: wmnotify and wmfiles under three Window Maker themes, in a private Xvfb
# and D-Bus session (nothing touches your real session or ~/GNUstep). Needs: make, Xvfb, dbus-run-session,
# ImageMagick (import, convert), build/ already built.   Usage: sh docs/make-screenshots.sh
set -eu
cd "$(dirname "$0")/.."
make -s all build/xprobe >/dev/null
OUT=docs/screenshots; mkdir -p "$OUT"
T=/tmp/wm-demo; rm -rf "$T"; mkdir -p "$T"; trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
ICON=$PWD/data/icons/hicolor/64x64/apps/wmfiles.png
FILES=$T/home/user; mkdir -p "$FILES/Documents/Letters" "$FILES/Pictures" "$FILES/Music"
for f in notes.txt todo.md report.pdf budget.ods; do echo x > "$FILES/$f"; done
for f in 2026-summer.jpg cat.png; do echo x > "$FILES/Pictures/$f"; done
echo x > "$FILES/Documents/thesis.tex"; echo x > "$FILES/Documents/Letters/to-ada.txt"

theme() {   # name titleA titleB titleText bodyA bodyB bodyText highlight highlightText border
cat > "$T/gs/Defaults/WindowMaker" <<EOT
{
  MenuTitleBack = (vgradient, "$2", "$3"); MenuTitleColor = "$4";
  MenuTextBack = (dgradient, "$5", "$6"); MenuTextColor = "$7";
  HighlightColor = "$8"; HighlightTextColor = "$9"; FrameBorderColor = "${10}";
  MenuTitleFont = "DejaVu Sans:bold:pixelsize=12"; MenuTextFont = "DejaVu Sans:pixelsize=12";
}
EOT
}
shot() {    # name ... theme args
  name=$1; shift; rm -rf "$T/gs"; mkdir -p "$T/gs/Defaults" "$T/home"; theme "$name" "$@"
  export HOME=$T/home WMAKER_USER_ROOT=$T/gs WMCONFIG_FILE=$T/wm.conf WMTHEME_GLOBAL=/nonexistent WMNOTIFY_HISTORY=$T/h WMFILES_OPENER=true XDG_DATA_HOME=$T/xdg
  : > "$T/wm.conf"
  D=$((60 + $(echo "$name" | cksum | cut -d' ' -f1) % 30)); export DISPLAY=:$D
  Xvfb :$D -screen 0 1040x470x24 >/dev/null 2>&1 & XP=$!; sleep 1
  convert -size 1040x470 gradient:'#5d6b7a-#2b333c' "$T/bg.png"; display_bg=1
  dbus-run-session -- sh -c '
    xsetroot -solid "#4a5560" 2>/dev/null || true
    build/wmfiles "'"$FILES"'" 2>/dev/null & sleep 1.2
    F=$(build/xprobe find WMFiles | cut -d" " -f1)
    build/xprobe click $F 60 34; sleep 0.3; build/xprobe click $F 300 34; sleep 0.5
    build/wmnotify 2>/dev/null & sleep 1
    build/wmnotify-send -a Mail -i "'"$ICON"'" "New message from <b>Ada</b>" "Lunch at <i>noon</i>? I booked the <u>corner table</u>." -t 0
    build/wmnotify-send -a backup -v 62 "Backup running" "Copying home to the NAS" -t 0
    build/wmnotify-send -a calendar -A open=Open -A snooze=Snooze "Meeting in 5 minutes" "Planning, room 2" -t 0 -w >/dev/null &
    build/wmnotify-send -a disk -u critical "Disk almost full" "Only 2% left on /home" -t 0
    sleep 1.5
    import -window root "'"$OUT"'/'"$name"'.png"' 
  kill $XP; wait $XP 2>/dev/null || true; pkill -f "build/wm(files|notify)" 2>/dev/null || true
}
shot nextstep  "#5a5a5a" "#1c1c1c" "#ffffff"  "#c8c8c8" "#9a9a9a" "#000000"  "#000000" "#ffffff" "#000000"
shot ocean     "#3a6ea5" "#14304f" "#ffffff"  "#dde6ee" "#a9bccd" "#10202e"  "#2a5d8f" "#ffffff" "#0b1b2b"
shot ember     "#8a3b1d" "#3b140a" "#ffe9d6"  "#f1dcc2" "#cfae85" "#2b1608"  "#8a3b1d" "#ffffff" "#2b0f05"
convert "$OUT/nextstep.png" "$OUT/ocean.png" "$OUT/ember.png" -append -resize 50% "$OUT/gallery.png" 2>/dev/null || true
echo "wrote $OUT/{nextstep,ocean,ember}.png"
