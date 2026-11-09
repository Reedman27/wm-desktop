#!/bin/sh
# Tests that need a display: wmdock list/watch against a fake EWMH window manager, plus headless icon/state/attrs checks.
# The X part is skipped (not failed) when Xvfb is missing.
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
build/wmdock attrs | grep -q 'vtool.Vtool.* = { Icon = ".*\.xpm"' || fail "icon: app whose only icon is an unusable SVG must get a generated tile, not nothing"

export WMCONFIG_FILE="$T/settings.conf"
build/wmsettings set focus sloppy && build/wmsettings set animations off || fail "settings set"
build/wmsettings set icon_size 500 2>/dev/null && fail "settings accepted out-of-range value"
build/wmsettings set focus bogus 2>/dev/null && fail "settings accepted bad choice"
[ "$(build/wmsettings get focus)" = "sloppy" ]                    || fail "settings get"
build/wmsettings set notify_corner bottom-left && [ "$(build/wmsettings get notify_corner)" = "bottom-left" ] || fail "notify_corner setting"
build/wmsettings set notify_corner middle 2>/dev/null && fail "notify_corner accepted a bad value"
build/wmsettings apply --dry-run | grep -q 'FocusMode Sloppy'     || fail "settings -> FocusMode"
build/wmsettings apply --dry-run | grep -q 'DisableAnimations YES' || fail "settings inverted bool"
printf '# keep me\nterminal = foot -e\n' > "$WMCONFIG_FILE"; build/wmsettings set focus click
grep -q '^# keep me' "$WMCONFIG_FILE"                              || fail "settings dropped comments"
build/wmmenud --once -o "$T/m" --dir "$FIX/share" 2>/dev/null; grep -q 'foot -e htop' "$T/m" || fail "wmmenud ignores shared terminal setting"
unset WMCONFIG_FILE

echo "all headless tests passed"

# ---- need X (Xvfb only; wmnotify and wmfiles are covered by run_gui.sh) ----
command -v Xvfb >/dev/null || { echo "SKIP: X tests (need Xvfb)"; exit 0; }
D=:$((90 + $$ % 9))
Xvfb $D -screen 0 800x400x24 >/dev/null 2>&1 & PIDS="$PIDS $!"; sleep 1
export DISPLAY=$D

build/wmdock watch > "$T/watch" & PIDS="$PIDS $!"; sleep 0.5
build/fakewm gnome-calculator Gnome-calculator Calculator 1500 & sleep 0.5
build/wmdock list | grep -q 'Gnome-calculator.org.gnome.Calculator.*Calculator' || fail "dock list match"
sleep 2
grep -q '^open.*org.gnome.Calculator' "$T/watch"  || fail "dock watch: open event"
grep -q '^close.*org.gnome.Calculator' "$T/watch" || fail "dock watch: close event"
echo "all X tests passed"
