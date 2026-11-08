#!/bin/sh
# Tests that need a display / session bus: wmdock, wmnotify, wmfiles, wmsettings, icons.
# Skipped (not failed) when Xvfb, dbus-run-session or xdotool are missing.
cd "$(dirname "$0")/.."
fail() { echo "FAIL: $1"; exit 1; }
FIX="$PWD/tests/fixtures"
T=$(mktemp -d); PIDS=""
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null; done; rm -rf "$T"; }
trap cleanup EXIT

# ---- no display needed ----
export HOME=/nonexistent XDG_DATA_HOME=/nonexistent XDG_DATA_DIRS="$FIX/share" WMDOCK_APPDIR="$FIX/share" LANG=C
build/wmdock state org.gnome.Calculator | grep -q 'Name = "gnome-calculator.Gnome-calculator"' || fail "dock state name"
build/wmdock state org.gnome.Calculator | grep -q 'Command = "gnome-calculator"'              || fail "dock state command"
build/wmdock attrs | grep -q 'icons/hicolor/128x128/apps/org.gnome.Calculator.png'            || fail "icon: should prefer larger raster over smaller"
build/wmdock attrs | grep -q vtool && fail "icon: svg-only app must not be given to Window Maker"

export WMCONFIG_FILE="$T/settings.conf"
build/wmsettings set focus sloppy && build/wmsettings set animations off || fail "settings set"
build/wmsettings set icon_size 500 2>/dev/null && fail "settings accepted out-of-range value"
build/wmsettings set focus bogus 2>/dev/null && fail "settings accepted bad choice"
[ "$(build/wmsettings get focus)" = "sloppy" ]                    || fail "settings get"
build/wmsettings apply --dry-run | grep -q 'FocusMode Sloppy'     || fail "settings -> FocusMode"
build/wmsettings apply --dry-run | grep -q 'DisableAnimations YES' || fail "settings inverted bool"
build/wmsettings apply -b wayland | grep -q 'nothing applied'      || fail "settings wayland honesty"
printf '# keep me\nterminal = foot -e\n' > "$WMCONFIG_FILE"; build/wmsettings set focus click
grep -q '^# keep me' "$WMCONFIG_FILE"                              || fail "settings dropped comments"
build/wmmenud --once -o "$T/m" --dir "$FIX/share" 2>/dev/null; grep -q 'foot -e htop' "$T/m" || fail "wmmenud ignores shared terminal setting"
unset WMCONFIG_FILE

echo "all headless tests passed"

# ---- need X ----
command -v Xvfb >/dev/null && command -v xdotool >/dev/null && command -v dbus-run-session >/dev/null \
  || { echo "SKIP: X tests (need Xvfb, xdotool, dbus-run-session)"; exit 0; }
D=:$((90 + $$ % 9))
Xvfb $D -screen 0 800x400x24 >/dev/null 2>&1 & PIDS="$PIDS $!"; sleep 1
export DISPLAY=$D

build/wmdock watch > "$T/watch" & PIDS="$PIDS $!"; sleep 0.5
build/fakewm gnome-calculator Gnome-calculator Calculator 1500 & sleep 0.5
build/wmdock list | grep -q 'Gnome-calculator.org.gnome.Calculator.*Calculator' || fail "dock list match"
sleep 2
grep -q '^open.*org.gnome.Calculator' "$T/watch"  || fail "dock watch: open event"
grep -q '^close.*org.gnome.Calculator' "$T/watch" || fail "dock watch: close event"

mkdir -p "$T/ft/Docs/Sub" "$T/ft/Music"; touch "$T/ft/Docs/a.txt" "$T/ft/Docs/Sub/deep.pdf" "$T/ft/.hid"
printf '#!/bin/sh\necho "$1" >> "%s/opened"\n' "$T" > "$T/opener"; chmod +x "$T/opener"
WMFILES_OPENER="$T/opener" build/wmfiles "$T/ft" & PIDS="$PIDS $!"; sleep 1
W=$(xdotool search --name "File Viewer" | head -1); xdotool windowmove "$W" 0 0
xdotool mousemove 50 33 click 1; sleep 0.2; xdotool mousemove 250 33 click 1; sleep 0.2   # Docs, then Sub
xdotool mousemove 450 33 click 1; sleep 0.1; xdotool click 1; sleep 0.5                    # double-click deep.pdf
grep -q 'Docs/Sub/deep.pdf' "$T/opened" || fail "wmfiles: double-click did not open the file"
kill $! 2>/dev/null

cat > "$T/n.sh" <<EOS
build/wmnotify 2>"$T/nerr" & NP=\$!
sleep 1
N="gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications --method org.freedesktop.Notifications"
\$N.GetServerInformation | grep -q wmnotify || exit 11
\$N.GetCapabilities | grep -q body || exit 12
\$N.Notify t 0 "" Hello "Body text" "[]" "{}" 800 | grep -q 'uint32 1' || exit 13
sleep 0.2; [ "\$(xdotool search --class wmnotify | wc -l)" -eq 1 ] || exit 14
sleep 2.5; [ "\$(xdotool search --class wmnotify | wc -l)" -eq 0 ] || exit 15
build/wmnotify 2>/dev/null; [ \$? -eq 1 ] || exit 16
kill \$NP
EOS
if command -v gdbus >/dev/null; then
  dbus-run-session -- sh "$T/n.sh" 2>/dev/null; rc=$?
  [ $rc -eq 0 ] || fail "wmnotify (step $rc: 11 info, 12 caps, 13 notify id, 14 popup shown, 15 popup expired, 16 second instance)"
else echo "SKIP: wmnotify (no gdbus)"; fi
echo "all X/D-Bus tests passed"
