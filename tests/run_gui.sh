#!/bin/sh
# GUI tests for wmnotify and wmfiles: private Xvfb + private session bus, no window manager, no xdotool.
# Checks that the popups really draw in the colours of a Window Maker theme file, and that changing the
# theme file changes the next draw. SKIPs (not fails) when Xvfb, dbus-daemon or gdbus are missing.
cd "$(dirname "$0")/.."
fail() { echo "FAIL: $1"; exit 1; }
for c in Xvfb dbus-daemon gdbus; do command -v $c >/dev/null || { echo "SKIP: GUI tests (need $c)"; exit 0; }; done
[ -x build/wmnotify ] || { echo "SKIP: GUI tests (wmnotify not built: needs libdbus headers)"; exit 0; }
T=$(mktemp -d); PIDS=""
cleanup() { for p in $PIDS; do kill $p 2>/dev/null; done; [ -n "$BUSPID" ] && kill $BUSPID 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

D=:$((70 + $$ % 20))
Xvfb $D -screen 0 1024x600x24 >/dev/null 2>&1 & PIDS="$PIDS $!"; sleep 1
export DISPLAY=$D
BUS=$(dbus-daemon --session --fork --print-address=1 --print-pid=2 2>"$T/pid") || fail "cannot start a private session bus"
BUSPID=$(cat "$T/pid"); export DBUS_SESSION_BUS_ADDRESS="$BUS"

export HOME="$T/home" WMAKER_USER_ROOT="$T/gs" WMTHEME_GLOBAL=/nonexistent
mkdir -p "$HOME" "$T/gs/Defaults"
theme() {   # theme MENUBACK TITLEBACK TEXT TITLETEXT HIGHLIGHT
  cat > "$T/gs/Defaults/WindowMaker" <<EOT
{
  // a comment, as real files have
  MenuTextBack = (dgradient, "$1", "#000000");
  MenuTitleBack = (solid, "$2");
  MenuTextColor = "$3";
  MenuTitleColor = "$4";
  HighlightColor = "$5";
  HighlightTextColor = black;
  MenuTextFont = "Sans:pixelsize=13";
  MenuTitleFont = "Sans:bold:pixelsize=13";
}
EOT
}
bump() { BN=$((${BN:-0} + 100)); touch -d "@$((1700000000 + BN))" "$T/gs/Defaults/WindowMaker"; }   # mtime has 1 s resolution: force a new one
P=build/xprobe
NB="gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications --method org.freedesktop.Notifications"

# ------------------------------------------------------------------ wmnotify
theme "#336699" "rgb:aa/00/00" "#ffffff" "#00ff00" "#ffcc00"; bump
build/wmnotify 2>"$T/nerr" & NP=$!; PIDS="$PIDS $NP"; sleep 1
grep -q 'theme from 1 Window Maker defaults file' "$T/nerr" || fail "wmnotify did not read the theme file: $(cat "$T/nerr")"
$NB.GetServerInformation | grep -q wmnotify || fail "GetServerInformation"
$NB.GetCapabilities | grep -q body            || fail "GetCapabilities"
$NB.Notify t "uint32 0" "" Hello "Body text that is long enough to need a little wrapping inside the popup" "@as []" "@a{sv} {}" "int32 20000" | grep -q 'uint32 1' || fail "Notify did not return id 1"
sleep 0.3
set -- $($P find WMNotify); [ $# -eq 6 ] || fail "expected one popup window, got: $*"
W1=$1; [ "$2" = 1 ] || fail "popup not mapped"
[ "$($P pixel $W1 300 5)" = "aa0000" ]  || fail "title bar is not MenuTitleBack (got $($P pixel $W1 300 5))"
H=$6; [ "$($P pixel $W1 300 $((H - 6)))" = "336699" ] || fail "body is not MenuTextBack (got $($P pixel $W1 300 $((H - 6))))"
[ "$($P count $W1 00ff00)" -gt 0 ]      || fail "no MenuTitleColor text pixels"
[ "$($P count $W1 ffffff)" -gt 0 ]      || fail "no MenuTextColor text pixels"

# a theme change is picked up by the next popup, not the running ones
theme "#224422" "#222266" "#ffff00" "#ff00ff" "#ffffff"; bump
$NB.Notify t "uint32 0" "" Second "" "@as []" "@a{sv} {}" "int32 20000" | grep -q 'uint32 2' || fail "second Notify id"
sleep 0.3
set -- $($P find WMNotify); [ $# -eq 12 ] || fail "expected two popups: $*"
NEW=""; for w in $1 $7; do [ "$($P pixel $w 300 5)" = "222266" ] && NEW=$w; done
[ -n "$NEW" ] || fail "second popup did not pick up the changed theme"
[ "$($P pixel $W1 300 5)" = "aa0000" ] || fail "first popup changed colour retroactively"
[ "$($P count $NEW ff00ff)" -gt 0 ] || fail "new MenuTitleColor missing"

# replaces_id updates in place; CloseNotification removes; critical urgency survives its timeout
$NB.Notify t "uint32 2" "" Replaced "now with body" "@as []" "@a{sv} {}" "int32 20000" | grep -q 'uint32 2' || fail "replaces_id must keep the id"
sleep 0.2; [ "$($P find WMNotify | wc -l)" -eq 2 ] || fail "replace created a new popup: $($P find WMNotify)"
$NB.CloseNotification 2 2>&1 | grep -q InvalidArgs || fail "wrongly typed CloseNotification must be an error"
$NB.CloseNotification "uint32 2" >/dev/null; sleep 0.2; [ "$($P find WMNotify | wc -l)" -eq 1 ] || fail "CloseNotification did not remove it"
$NB.CloseNotification "uint32 1" >/dev/null; sleep 0.2
$NB.Notify t "uint32 0" "" Urgent "stays" "@as []" "@a{sv} {'urgency': <byte 2>}" "int32 300" | grep -q 'uint32' || fail "critical Notify"
$NB.Notify t "uint32 0" "" Normal "goes" "@as []" "@a{sv} {'urgency': <byte 1>}" "int32 300" >/dev/null
sleep 1.5; [ "$($P find WMNotify | wc -l)" -eq 1 ] || fail "after the timeout only the critical popup should remain (got $($P find WMNotify | wc -l))"
W=$($P find WMNotify | cut -d' ' -f1); $P click $W 20 10; sleep 0.3
[ "$($P find WMNotify | wc -l)" -eq 0 ] || fail "click did not dismiss the critical popup"
build/wmnotify 2>/dev/null; [ $? -eq 1 ] || fail "second instance must refuse to start"
kill -0 $NP 2>/dev/null || fail "wmnotify died"
grep -qE "Sanitizer|runtime error" "$T/nerr" && fail "wmnotify sanitizer report: $(cat "$T/nerr")"
echo "ok: wmnotify"

# ------------------------------------------------------------------- wmfiles
mkdir -p "$T/ft/Docs" "$T/ft/Music"; touch "$T/ft/Docs/a.txt"
theme "#336699" "rgb:aa/00/00" "#ffffff" "#00ff00" "#ffcc00"; bump
build/wmfiles "$T/ft" 2>"$T/ferr" & FP=$!; PIDS="$PIDS $FP"; sleep 1
set -- $($P find WMFiles); [ $# -eq 6 ] || fail "wmfiles window missing: $(cat "$T/ferr")"
F=$1
[ "$($P pixel $F 590 3)" = "aa0000" ]  || fail "wmfiles title bar is not MenuTitleBack"
[ "$($P pixel $F 190 300)" = "336699" ] || fail "wmfiles body is not MenuTextBack (got $($P pixel $F 190 300))"
$P click $F 50 32; sleep 0.3
[ "$($P pixel $F 100 32)" = "ffcc00" ]  || fail "selected row is not HighlightColor (got $($P pixel $F 100 32))"
theme "#224422" "#222266" "#ffff00" "#ff00ff" "#ffffff"; bump
$P click $F 50 32; sleep 0.3              # any event redraws
[ "$($P pixel $F 590 3)" = "222266" ]  || fail "wmfiles did not follow the theme change"
# double click opens a file with $WMFILES_OPENER
kill $FP 2>/dev/null; sleep 0.3
mkdir -p "$T/ft/Docs/Sub"; touch "$T/ft/Docs/Sub/deep.pdf"
printf '#!/bin/sh\necho "$1" >> "%s/opened"\n' "$T" > "$T/opener"; chmod +x "$T/opener"
WMFILES_OPENER="$T/opener" build/wmfiles "$T/ft" 2>/dev/null & FP=$!; PIDS="$PIDS $FP"; sleep 1
F=$($P find WMFiles | cut -d' ' -f1)
$P click $F 50 32; sleep 0.2; $P click $F 250 32; sleep 0.2     # Docs, then Sub
$P click $F 450 32; sleep 0.1; $P click $F 450 32; sleep 0.5    # double-click deep.pdf
grep -q 'Docs/Sub/deep.pdf' "$T/opened" || fail "wmfiles: double-click did not open the file"
$P key $F q; sleep 0.5
kill -0 $FP 2>/dev/null && fail "q did not quit wmfiles"
grep -qE "Sanitizer|runtime error" "$T/ferr" && fail "wmfiles sanitizer report"
echo "ok: wmfiles"
echo "all GUI tests passed"
