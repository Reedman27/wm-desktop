#!/bin/sh
# GUI tests for wmnotify and wmfiles: private Xvfb + private session bus, no window manager, no xdotool.
# Checks that the popups really draw in the colours of a Window Maker theme file, and that changing the
# theme file changes the next draw. SKIPs (not fails) when Xvfb, dbus-daemon or gdbus are missing.
cd "$(dirname "$0")/.."
fail() { echo "FAIL: $1"; exit 1; }
for c in Xvfb dbus-daemon gdbus; do command -v $c >/dev/null || { echo "SKIP: GUI tests (need $c)"; exit 0; }; done
[ -x build/wmnotify ] && [ -x build/wmfiles ] || { echo "SKIP: GUI tests (wmnotify/wmfiles not built: need libdbus and Xft headers)"; exit 0; }
T=$(mktemp -d); PIDS=""
cleanup() { for p in $PIDS; do kill $p 2>/dev/null; done; [ -n "$BUSPID" ] && kill $BUSPID 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

D=:$((70 + $$ % 20))
Xvfb $D -screen 0 1024x600x24 >/dev/null 2>&1 & PIDS="$PIDS $!"; sleep 1
export DISPLAY=$D
BUS=$(dbus-daemon --session --fork --print-address=1 --print-pid=2 2>"$T/pid") || fail "cannot start a private session bus"
BUSPID=$(cat "$T/pid"); export DBUS_SESSION_BUS_ADDRESS="$BUS"

export HOME="$T/home" WMAKER_USER_ROOT="$T/gs" WMTHEME_GLOBAL=/nonexistent WMCONFIG_FILE="$T/wm.conf" WMNOTIFY_HISTORY="$T/notify-history"
mkdir -p "$HOME" "$T/gs/Defaults"
theme() {   # theme MENUBACK TITLEBACK TEXT TITLETEXT HIGHLIGHT
  cat > "$T/gs/Defaults/WindowMaker" <<EOT
{
  // a comment, as real files have
  MenuTextBack = (${GRAD:-solid}, "$1"${GRAD:+, "#000000"});
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
$NB.Notify t "uint32 0" "" Hello "Body text that is long enough to need a little wrapping inside the popup" "@as []" "@a{sv} {}" "int32 120000" | grep -q 'uint32 1' || fail "Notify did not return id 1"
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
[ "$($P pixel $W1 300 5)" = "222266" ] || fail "open popup did not restyle after a theme change (got $($P pixel $W1 300 5))"
[ "$($P count $NEW ff00ff)" -gt 0 ] || fail "new MenuTitleColor missing"

# real gradients: a dgradient body runs from its first colour (top-left) to its second (bottom-right)
GRAD=dgradient theme "#ff0000" "#222266" "#ffffff" "#ffffff" "#ffffff"; bump
$NB.Notify t "uint32 0" "" Gradient "a body" "@as []" "@a{sv} {}" "int32 20000" | grep -q 'uint32 3' || fail "gradient Notify id"
sleep 0.4
for w in $($P find WMNotify | cut -d' ' -f1); do
  set -- $($P find WMNotify | grep "^$w "); GH=$6; GW=$5
  A=$($P pixel $w 3 $((GH - 20))); B=$($P pixel $w $((GW - 4)) $((GH - 4)))
  [ "$A" != "$B" ] && { GOT=1; break; }
done
[ -n "${GOT:-}" ] || fail "dgradient body is a flat colour"
theme "#336699" "rgb:aa/00/00" "#ffffff" "#00ff00" "#ffcc00"; bump; sleep 1.2
$NB.CloseNotification "uint32 3" >/dev/null; sleep 0.3; [ "$($P find WMNotify | wc -l)" -eq 2 ] || fail "gradient popup not closed: $($P find WMNotify)"

# replaces_id updates in place; CloseNotification removes; critical urgency survives its timeout
$NB.Notify t "uint32 2" "" Replaced "now with body" "@as []" "@a{sv} {}" "int32 20000" | grep -q 'uint32 2' || fail "replaces_id must keep the id"
sleep 0.2; [ "$($P find WMNotify | wc -l)" -eq 2 ] || fail "replace created a new popup: $($P find WMNotify)"
$NB.CloseNotification 2 2>&1 | grep -q InvalidArgs || fail "wrongly typed CloseNotification must be an error"
$NB.CloseNotification "uint32 2" >/dev/null; sleep 0.2; [ "$($P find WMNotify | wc -l)" -eq 1 ] || fail "CloseNotification did not remove it: $($P find WMNotify)"
$NB.CloseNotification "uint32 1" >/dev/null; sleep 0.2
$NB.Notify t "uint32 0" "" Urgent "stays" "@as []" "@a{sv} {'urgency': <byte 2>}" "int32 300" | grep -q 'uint32' || fail "critical Notify"
$NB.Notify t "uint32 0" "" Normal "goes" "@as []" "@a{sv} {'urgency': <byte 1>}" "int32 300" >/dev/null
sleep 1.5; [ "$($P find WMNotify | wc -l)" -eq 1 ] || fail "after the timeout only the critical popup should remain (got $($P find WMNotify | wc -l))"
W=$($P find WMNotify | cut -d' ' -f1); $P click $W 20 10; sleep 0.3
[ "$($P find WMNotify | wc -l)" -eq 0 ] || fail "click did not dismiss the critical popup"

# action buttons: a real click on a button must emit ActionInvoked with its key, then close the pop-up
gdbus monitor --session --dest org.freedesktop.Notifications > "$T/mon" 2>&1 & MP=$!; PIDS="$PIDS $MP"; sleep 0.5
$NB.Notify t "uint32 0" "" Question "Proceed?" "['yes', 'Yes', 'no', 'No']" "@a{sv} {}" "int32 60000" | grep -q 'uint32' || fail "Notify with actions"
sleep 0.4; set -- $($P find WMNotify); [ $# -eq 6 ] || fail "action popup missing: $*"
AW=$1; AH=$6; $P click $AW 40 $((AH - 18)); sleep 0.6
grep -q "ActionInvoked" "$T/mon" && grep -q "'yes'" "$T/mon" || fail "button click did not emit ActionInvoked(yes): $(cat "$T/mon")"
[ "$($P find WMNotify | wc -l)" -eq 0 ] || fail "popup still open after an action"

# wmnotify-send -R: picking the action runs the command, and the exit status is the command's
rm -f "$T/ran"
build/wmnotify-send -a runner -A go=Go -R "go=touch $T/ran; exit 7" "Run it" "click Go" > "$T/runout" 2>&1 & RP=$!
sleep 1; set -- $($P find WMNotify); [ $# -eq 6 ] || fail "-R popup missing: $*"
$P click $1 40 $(($6 - 18)); wait $RP; RC=$?
[ -f "$T/ran" ] && [ "$RC" -eq 7 ] && grep -q '^go$' "$T/runout" || fail "wmnotify-send -R (ran=$([ -f "$T/ran" ] && echo yes || echo no) rc=$RC out=$(cat "$T/runout"))"

# markup: <b>/<i>/<u> are styles, other tags vanish, hostile control bytes are ignored; underline adds a solid line
tc() { $NB.Notify mk "uint32 0" "" "Markup" "$1" "@as []" "@a{sv} {}" "int32 60000" >/dev/null; sleep 0.4
       W=$($P find WMNotify | tail -1 | cut -d' ' -f1); $P count $W "${2:-ffff00}"; }
for w in $($P find WMNotify | cut -d' ' -f1); do $P click $w 20 10 2>/dev/null; done; sleep 0.3
for w in $($P find WMNotify | cut -d' ' -f1); do $P click $w 20 10 2>/dev/null; done; sleep 0.3
theme "#336699" "rgb:aa/00/00" "#ffff00" "#ffffff" "#00ff00"; bump; sleep 1.2
PLAIN=$(tc "WWWWWWWW"); for w in $($P find WMNotify | cut -d' ' -f1); do $P click $w 20 10 2>/dev/null; done; sleep 0.3
UNDER=$(tc "<u>WWWWWWWW</u>"); for w in $($P find WMNotify | cut -d' ' -f1); do $P click $w 20 10 2>/dev/null; done; sleep 0.3
BOLD=$(tc "<b>WWWWWWWW</b>"); for w in $($P find WMNotify | cut -d' ' -f1); do $P click $w 20 10 2>/dev/null; done; sleep 0.3
[ "$UNDER" -gt "$PLAIN" ] || fail "underline did not add pixels (plain $PLAIN, underlined $UNDER)"
[ "$BOLD" -ge "$PLAIN" ] || fail "bold is lighter than plain (plain $PLAIN, bold $BOLD)"
MARK=$(tc "<span class='x'>WWWWWWWW</span><img src=x/>"); for w in $($P find WMNotify | cut -d' ' -f1); do $P click $w 20 10 2>/dev/null; done; sleep 0.3
[ "$MARK" = "$PLAIN" ] || fail "unknown tags changed the rendering ($MARK vs $PLAIN)"
HOST=$(tc "$(printf 'WWWW\001WWWW')"); for w in $($P find WMNotify | cut -d' ' -f1); do $P click $w 20 10 2>/dev/null; done; sleep 0.3
[ "$HOST" = "$PLAIN" ] || fail "a raw control byte in the body changed the rendering ($HOST vs $PLAIN)"
kill -0 $NP 2>/dev/null || fail "wmnotify died on markup"

# hover pauses the timeout, leaving restarts it with a short grace period
$NB.Notify t "uint32 0" "" Hover "stay while hovered" "@as []" "@a{sv} {}" "int32 1500" >/dev/null; sleep 0.4
HW=$($P find WMNotify | cut -d' ' -f1); $P enter $HW; sleep 2.5
[ "$($P find WMNotify | wc -l)" -eq 1 ] || fail "hovered popup expired"
$P leave $HW; sleep 3.5
[ "$($P find WMNotify | wc -l)" -eq 0 ] || fail "popup did not expire after the pointer left"

# do-not-disturb and per-app rules: nothing pops up, everything is kept in the history; critical still shows
printf 'notify_dnd = on\n' > "$WMCONFIG_FILE"
$NB.Notify chatty "uint32 0" "" "While DND" "quiet" "@as []" "@a{sv} {}" "int32 60000" | grep -q 'uint32' || fail "Notify during DND must still return an id"
sleep 0.4; [ "$($P find WMNotify | wc -l)" -eq 0 ] || fail "DND popup appeared"
$NB.Notify chatty "uint32 0" "" "Critical in DND" "shows" "@as []" "@a{sv} {'urgency': <byte 2>}" "int32 0" >/dev/null
sleep 0.4; [ "$($P find WMNotify | wc -l)" -eq 1 ] || fail "critical must show during DND"
$P click $($P find WMNotify | cut -d' ' -f1) 20 10; sleep 0.3
printf 'notify_dnd = off\nnotify_mute = noisy, other\nnotify_app_timeout = slowapp:60000\n' > "$WMCONFIG_FILE"
$NB.Notify noisy "uint32 0" "" "Muted app" "x" "@as []" "@a{sv} {}" "int32 60000" >/dev/null
sleep 0.4; [ "$($P find WMNotify | wc -l)" -eq 0 ] || fail "muted app popup appeared"
$NB.Notify Noisy "uint32 0" "" "Muted app, other case" "x" "@as []" "@a{sv} {}" "int32 60000" >/dev/null
sleep 0.3; [ "$($P find WMNotify | wc -l)" -eq 0 ] || fail "app mute is case-sensitive"
$NB.Notify slowapp "uint32 0" "" "Slow" "per-app timeout" "@as []" "@a{sv} {}" "int32 -1" >/dev/null; sleep 0.3
[ "$($P find WMNotify | wc -l)" -eq 1 ] || fail "unmuted app popup missing"
sleep 5.5; [ "$($P find WMNotify | wc -l)" -eq 1 ] || fail "notify_app_timeout ignored (popup left after the 5 s default)"
$NB.CloseNotification "uint32 $($NB.Notify slowapp "uint32 0" "" Tmp "" "@as []" "@a{sv} {}" "int32 100" | sed 's/[^0-9]//g')" >/dev/null 2>&1
$P click $($P find WMNotify | head -1 | cut -d' ' -f1) 20 10; sleep 0.3; $P click $($P find WMNotify | head -1 | cut -d' ' -f1) 20 10 2>/dev/null; sleep 0.3
: > "$WMCONFIG_FILE"
build/wmnotify-send -H 30 > "$T/hist" || fail "wmnotify-send -H"
grep -q "\[dnd\] chatty: While DND" "$T/hist"        || fail "history lacks the DND notification: $(cat "$T/hist")"
grep -q "\[muted\] noisy: Muted app" "$T/hist"      || fail "history lacks the muted notification"
grep -q "t: Question" "$T/hist"        || fail "history lacks a shown notification"

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

# file operations (trash, new folder, rename, copy, cut/paste), driven by keys
kill $FP 2>/dev/null; sleep 0.3
FO="$T/fo"; mkdir -p "$FO/zdir"; echo a > "$FO/alpha.txt"; echo b > "$FO/beta.txt"
XDG_DATA_HOME="$T/xdg" build/wmfiles "$FO" 2>"$T/ferr2" & FP=$!; PIDS="$PIDS $FP"; sleep 1
F=$($P find WMFiles | cut -d' ' -f1); [ -n "$F" ] || fail "wmfiles (ops) did not start: $(cat "$T/ferr2")"
k() { for x in "$@"; do $P key $F "$x"; sleep 0.08; done; sleep 0.15; }
k Down Down Delete                                             # rows: zdir, alpha.txt, beta.txt -> trash alpha.txt
[ ! -e "$FO/alpha.txt" ] && [ -f "$T/xdg/Trash/files/alpha.txt" ] || fail "Delete did not move alpha.txt to the trash"
grep -q 'Path=.*alpha.txt' "$T/xdg/Trash/info/alpha.txt.trashinfo" || fail "no .trashinfo for the trashed file"
k n n e w Return                                               # new folder "new"
[ -d "$FO/new" ] || fail "n did not create a folder"
k F2 BackSpace BackSpace BackSpace x y Return                  # rename it to "xy"
[ -d "$FO/xy" ] && [ ! -e "$FO/new" ] || fail "F2 rename failed"
k F2 BackSpace BackSpace z d i r Return                        # renaming onto an existing name must not overwrite
[ -d "$FO/xy" ] && [ -d "$FO/zdir" ] || fail "rename onto an existing name replaced something"
k F2 Escape                                                    # cancelling changes nothing
[ -d "$FO/xy" ] || fail "Escape did not cancel rename"
k Down Down ctrl+c Up Right ctrl+v                             # copy beta.txt into zdir
[ -f "$FO/zdir/beta.txt" ] && [ -f "$FO/beta.txt" ] || fail "copy/paste failed"
k Left Down ctrl+x Up Up Right ctrl+v                          # cut beta.txt, paste into xy
[ -f "$FO/xy/beta.txt" ] && [ ! -e "$FO/beta.txt" ] || fail "cut/paste failed"
# undo walks back through the operations above: move, copy, rename, new folder, trash
k ctrl+z; [ -f "$FO/beta.txt" ] && [ ! -e "$FO/xy/beta.txt" ] || fail "undo of cut/paste"
k ctrl+z; [ ! -e "$FO/zdir/beta.txt" ] || fail "undo of copy"
k ctrl+z; [ -d "$FO/new" ] && [ ! -e "$FO/xy" ] || fail "undo of rename"
k ctrl+z; [ ! -e "$FO/new" ] || fail "undo of new folder"
k ctrl+z; [ -f "$FO/alpha.txt" ] && [ ! -e "$T/xdg/Trash/files/alpha.txt" ] || fail "undo of trash"
k ctrl+z; kill -0 $FP 2>/dev/null || fail "undo with an empty history crashed wmfiles"

# the columns follow the disk: files created behind its back appear without any key press
BEFORE=$($P count $F ffff00)
for i in 1 2 3 4 5; do : > "$FO/external-file-number-$i.txt"; done; sleep 1.2
AFTER=$($P count $F ffff00); [ "$AFTER" -gt "$BEFORE" ] || fail "wmfiles did not notice new files (text pixels $BEFORE -> $AFTER)"

# trash view: t switches, r restores to the original place, t goes back
: > "$FO/gamma.txt"; sleep 1
k Up Up Up Up Up Up Up Up Up Up Up Up Up Up Up Down Down Down Down Down Down Down Down Down   # zdir, alpha, beta, external x5, gamma
k Delete; [ ! -e "$FO/gamma.txt" ] && [ -f "$T/xdg/Trash/files/gamma.txt" ] || fail "gamma.txt not trashed"
k t Up Up Up Down r
[ -f "$FO/gamma.txt" ] && [ ! -e "$T/xdg/Trash/files/gamma.txt" ] || fail "r in the trash view did not restore gamma.txt"
[ ! -e "$T/xdg/Trash/info/gamma.txt.trashinfo" ] || fail "restore left the .trashinfo behind"
k t; kill -0 $FP 2>/dev/null || fail "wmfiles died in the trash view: $(cat "$T/ferr2")"
kill -0 $FP 2>/dev/null || fail "wmfiles died during file operations: $(cat "$T/ferr2")"
grep -qE "Sanitizer|runtime error" "$T/ferr2" && fail "wmfiles ops sanitizer report"
# several items at once: Space / Ctrl+click / Shift+click marks, one undo step per operation, Return and "open with"
kill $FP 2>/dev/null; sleep 0.3
MS="$T/ms"; mkdir -p "$MS/d"; for f in m1 m2 m3 m4 ow; do echo "$f" > "$MS/$f.txt"; done
WMFILES_OPENER=touch XDG_DATA_HOME="$T/xdg" build/wmfiles "$MS" 2>"$T/ferr3" & FP=$!; PIDS="$PIDS $FP"; sleep 1
F=$($P find WMFiles | cut -d' ' -f1); [ -n "$F" ] || fail "wmfiles (multi) did not start: $(cat "$T/ferr3")"
# row geometry from the pixels: select row 0, then measure the highlighted band (works for any theme/font)
$P click $F 60 32; sleep 0.3; HL=$($P pixel $F 60 32); RT=32; while [ "$($P pixel $F 60 $((RT - 1)))" = "$HL" ]; do RT=$((RT - 1)); done
RB=32; while [ "$($P pixel $F 60 $((RB + 1)))" = "$HL" ]; do RB=$((RB + 1)); done; RH=$((RB - RT + 1)); [ "$RH" -ge 10 ] || fail "could not measure the row height ($RT..$RB)"
row() { echo $((RT + $1 * RH + RH / 2)); }                        # y of the middle of row N (0 = first row)
k Down space space Delete                                        # cursor: d, m1; mark m1, m2; trash both
[ ! -e "$MS/m1.txt" ] && [ ! -e "$MS/m2.txt" ] && [ -e "$MS/m3.txt" ] || fail "Space-marked items were not trashed together"
k ctrl+z; [ -e "$MS/m1.txt" ] && [ -e "$MS/m2.txt" ] || fail "one Ctrl+Z must restore every item of the operation"
$P click $F 60 $(row 1) "1+ctrl"; $P click $F 60 $(row 3) "1+ctrl"; sleep 0.2          # mark m1 and m3
k ctrl+c; $P click $F 60 $(row 0); sleep 0.3; k Right ctrl+v      # into d
[ -f "$MS/d/m1.txt" ] && [ -f "$MS/d/m3.txt" ] && [ ! -e "$MS/d/m2.txt" ] && [ ! -e "$MS/d/m4.txt" ] || fail "pasting two marked items copied the wrong set: $(ls "$MS/d")"
k ctrl+z; [ ! -e "$MS/d/m1.txt" ] && [ ! -e "$MS/d/m3.txt" ] || fail "undo of a two-item paste"
$P click $F 60 $(row 1); $P click $F 60 $(row 3) "1+shift"; sleep 0.2; k Delete        # range m1..m3
[ ! -e "$MS/m1.txt" ] && [ ! -e "$MS/m2.txt" ] && [ ! -e "$MS/m3.txt" ] && [ -e "$MS/m4.txt" ] || fail "Shift+click range delete"
touch -d 2001-01-01 "$MS/m4.txt" "$MS/ow.txt"                    # rows now: d, m4, ow
$P click $F 60 $(row 1); k Return; sleep 0.3
[ "$(stat -c %Y "$MS/m4.txt")" -gt 1500000000 ] || fail "Return did not open the file with \$WMFILES_OPENER"
$P click $F 60 $(row 2); k o t o u c h Return; sleep 0.3
[ "$(stat -c %Y "$MS/ow.txt")" -gt 1500000000 ] || fail "open with (o, typed command) did not run"
kill -0 $FP 2>/dev/null || fail "wmfiles died in the multi-select tests: $(cat "$T/ferr3")"
echo "ok: wmfiles"
echo "all GUI tests passed"
