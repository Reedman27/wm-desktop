#!/bin/sh
# Smoke tests for WMApps. Uses an isolated fixture dir; XDG vars are pointed at nothing.
set -e
cd "$(dirname "$0")/.."
export HOME=/nonexistent XDG_DATA_HOME=/nonexistent XDG_DATA_DIRS=/nonexistent LANG=C
W="build/wmapps"; F="--dir tests/fixtures/share"
fail() { echo "FAIL: $1"; exit 1; }
$W list $F > build/list.txt
grep -q "Calculator" build/list.txt            || fail "calculator missing"
grep -q "vendor-tool" build/list.txt           || fail "vendor subdir id prefix"
! grep -q "Hidden Thing" build/list.txt        || fail "NoDisplay shown"
! grep -q "GNOME Only" build/list.txt          || fail "OnlyShowIn=GNOME shown"
! grep -q "Not Installed" build/list.txt       || fail "TryExec ignored"
[ "$(wc -l < build/list.txt)" -eq 4 ]          || fail "expected 4 apps"
LANG=de_DE.UTF-8 $W list $F | grep -q Taschenrechner || fail "localized name"
$W menu $F > build/x11.menu
grep -q 'SHEXEC, "xterm -e htop"' build/x11.menu     || fail "terminal wrap"
grep -q 'gnome-calculator")' build/x11.menu          || fail "field code not stripped"
grep -qF "sh -c 'echo hi \\\\%; sleep 1'" build/x11.menu || fail "x11: % must be escaped for Window Maker's ExpandOptions"
python3 - <<'PY'
import re
s = open('build/x11.menu').read()
s = re.sub(r'"(?:\\.|[^"\\])*"', '""', s)         # drop strings, then check balance
s = re.sub(r'//.*', '', s)
assert s.count('(') == s.count(')'), 'unbalanced parens'
PY
$W json $F | python3 -c "import json,sys; d=json.load(sys.stdin); assert len(d)==4 and d[0]['id']"
# exec / launch / skipped CLI
build/wmapps exec org.gnome.Calculator "/tmp/a b" $F | tr '\n' '|' | grep -qF 'gnome-calculator|file:///tmp/a%20b|' || fail "exec: %U must become a file:// URI"
build/wmapps exec htop -t "kitty -e" $F | tr '\n' '|' | grep -qF 'kitty|-e|htop|' || fail "exec: terminal prefix"
build/wmapps exec nonexistent-app $F 2>/dev/null && fail "exec: unknown app must fail"
build/wmapps skipped $F > build/skipped.txt
[ ! -s build/skipped.txt ] || fail "skipped: fixtures are all valid, got: $(cat build/skipped.txt)"
# icons: every app gets a PNG/XPM, nothing is svg
IC=$(mktemp -d); IC_OUT=$(XDG_CACHE_HOME="$IC" $W icons $F) || fail "icons: an app was left without an icon"
[ "$(echo "$IC_OUT" | wc -l)" -eq 4 ]                                  || fail "icons: one line per app"
echo "$IC_OUT" | grep -q '^theme .*org.gnome.Calculator.*128x128'      || fail "icons: calculator should use its theme icon"
echo "$IC_OUT" | grep -q '^generated .*htop .*\.xpm$'                 || fail "icons: htop has no icon and should get a drawn one"
! echo "$IC_OUT" | grep -qi '\.svg$'                                  || fail "icons: svg handed out"
XDG_CACHE_HOME=/proc/nope $W icons $F >/dev/null 2>&1 && fail "icons: unwritable cache must report missing icons"
rm -rf "$IC"
echo "all WMApps tests passed"

# ---- wmmenud: live inotify behaviour ----
T=$(mktemp -d); trap 'kill $PID 2>/dev/null || true; rm -rf "$T"' EXIT
mkdir -p "$T/share/applications"; cp tests/fixtures/share/applications/*.desktop "$T/share/applications/"
build/wmmenud -o "$T/menu" --dir "$T/share" 2> "$T/log" & PID=$!
sleep 1
grep -q 'SHEXEC, "gnome-calculator"' "$T/menu"  || fail "daemon: initial menu"
grep -q '"Window Maker"' "$T/menu"              || fail "daemon: root template"
grep -q 'WORKSPACE_MENU' "$T/menu"              || fail "daemon: root entries"
printf '[Desktop Entry]\nType=Application\nName=Zed Editor\nExec=zed\nCategories=Development;\n' > "$T/share/applications/zed.desktop"
sleep 2
grep -q 'Zed Editor' "$T/menu"                  || fail "daemon: install not picked up"
M1=$(stat -c %Y.%N "$T/menu")
touch "$T/share/applications/zed.desktop"; sleep 2      # no content change
[ "$M1" = "$(stat -c %Y.%N "$T/menu")" ]       || fail "daemon: rewrote menu without change"
rm "$T/share/applications/zed.desktop"; sleep 2
! grep -q 'Zed Editor' "$T/menu"                || fail "daemon: removal not picked up"
mkdir "$T/share/applications/newvendor"
printf '[Desktop Entry]\nType=Application\nName=Late Vendor\nExec=lv\n' > "$T/share/applications/newvendor/lv.desktop"
sleep 2
grep -q 'Late Vendor' "$T/menu"                 || fail "daemon: new vendor subdir"
[ -z "$(ls "$T" | grep tmp)" ]                  || fail "daemon: temp file left behind"
mkdir -p "$T/share/applications/x/y/z"
printf '[Desktop Entry]\nType=Application\nName=Deep One\nExec=deep\n' > "$T/share/applications/x/y/z/d.desktop"
sleep 2
grep -q 'Deep One' "$T/menu"                    || fail "daemon: three-level nested dir not picked up"
printf '[Desktop Entry]\nType=Application\nName=Deep Two\nExec=deep2\n' > "$T/share/applications/x/y/z/e.desktop"
sleep 2
grep -q 'Deep Two' "$T/menu"                    || fail "daemon: file added in nested dir created after startup"
kill $PID; wait $PID 2>/dev/null || true
echo "all wmmenud tests passed"

# ---- wmgfx ----
G="build/wmgfx"; export WMGFX_SYSFS="$PWD/tests/fixtures/sysfs"
$G detect | grep -q '^driver: amdgpu'           || fail "gfx: amdgpu not detected"
$G detect | grep -q '^driver: nvidia'           || fail "gfx: second card not detected"
[ "$($G detect | grep -c '^driver')" -eq 2 ]    || fail "gfx: connector dirs counted as cards"
$G detect | grep -q '^primary: amdgpu'          || fail "gfx: hybrid should prefer amdgpu"
$G xorg | grep -q 'Option "TearFree" "true"'    || fail "gfx: amdgpu TearFree"
$G picom | grep -q 'vsync = true'               || fail "gfx: picom vsync"
H=$(mktemp -d); HOME=$H XDG_CONFIG_HOME= $G apply 2>/dev/null
[ -s "$H/.config/picom/picom.conf" ]            || fail "gfx: apply did not write picom.conf"
rm -rf "$H"
WMGFX_SYSFS=/nonexistent $G detect | grep -q 'primary: none' || fail "gfx: no-gpu case"
echo "all wmgfx tests passed"
