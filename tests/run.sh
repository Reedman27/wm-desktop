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
# icons: every app gets one; the project's own app ships a real icon
C="$PWD/build/icon-cache"; rm -rf "$C"; export XDG_CACHE_HOME="$C"
IC=$(build/wmapps icons $F)
echo "$IC" | grep -q ' 0 without icon' || fail "icons: some app has no icon: $IC"
echo "$IC" | grep -q '^file .*org.gnome.Calculator' || fail "icons: theme icon not used for Calculator"
echo "$IC" | grep -q '^generated .*htop' || fail "icons: app without any icon must get a generated tile"
XDG_DATA_DIRS="$PWD/data" build/wmapps icons --dir "$PWD/data" | grep -A1 '^file  *wmfiles' | grep -q 'wmfiles\.png' || fail "icons: wmfiles must resolve to its shipped PNG"
WMDOCK_APPDIR="$PWD/tests/fixtures/share" build/wmdock attrs | grep -q 'AlwaysUserIcon' && fail "attrs: AlwaysUserIcon only with --always"
WMDOCK_APPDIR="$PWD/tests/fixtures/share" build/wmdock attrs --always | grep -q 'AlwaysUserIcon = Yes' || fail "attrs --always"
[ "$(WMDOCK_APPDIR="$PWD/tests/fixtures/share" build/wmdock attrs | grep -c ' = { Icon = ')" -eq 4 ] || fail "attrs: every app (4) must get an Icon entry"
WMDOCK_APPDIR="$PWD/tests/fixtures/share" build/wmdock state org.gnome.Calculator nonexistent-app 2>/dev/null | tr '\n' '|' | grep -qF '},|);' && fail "state: trailing comma when a later id is unknown"
unset XDG_CACHE_HOME
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

# ---- wmdock apply-icons ----
export XDG_CACHE_HOME="$PWD/build/icon-cache"
A=$(mktemp -d); mkdir -p "$A/Defaults"; AF="$A/Defaults/WMWindowAttributes"
export WMAKER_USER_ROOT="$A" WMDOCK_APPDIR="$PWD/tests/fixtures/share"
D=build/wmdock
printf '{\n  "mine.Mine" = { Icon = "/my/own.png"; };\n}\n' > "$AF"
$D apply-icons --dry-run 2>/dev/null | grep -q ' = { Icon = '       || fail "apply-icons: dry run prints nothing"
[ "$(wc -l < "$AF")" -eq 3 ]                                           || fail "apply-icons: dry run changed the file"
$D apply-icons 2>/dev/null                                             || fail "apply-icons failed"
grep -q '"mine.Mine" = { Icon = "/my/own.png"; };' "$AF"              || fail "apply-icons: existing entry lost"
[ "$(grep -c ' = { Icon = ' "$AF")" -eq 5 ]                            || fail "apply-icons: expected 1 existing + 4 new entries"
ls "$AF".bak-* >/dev/null 2>&1                                         || fail "apply-icons: no backup written"
grep -q '/my/own.png' "$AF".bak-*                                      || fail "apply-icons: backup does not hold the original"
[ "$(head -c1 "$AF")" = "{" ] && [ "$(tail -n1 "$AF")" = "}" ]         || fail "apply-icons: result is not a dict"
cp "$AF" "$A/before"; $D apply-icons 2>/dev/null; cmp -s "$AF" "$A/before" || fail "apply-icons: second run must change nothing"
echo 'garbage, not a dict' > "$AF"; cp "$AF" "$A/before"
$D apply-icons 2>/dev/null && fail "apply-icons: must refuse an unparseable file"
cmp -s "$AF" "$A/before"                                               || fail "apply-icons: touched an unparseable file"
rm -f "$AF"; $D apply-icons 2>/dev/null                                || fail "apply-icons: creating a new file failed"
[ "$(grep -c ' = { Icon = ' "$AF")" -eq 4 ]                            || fail "apply-icons: new file entries"
rm -rf "$A"; unset WMAKER_USER_ROOT WMDOCK_APPDIR
echo "all apply-icons tests passed"

# wm-autostart: idempotent, preserves the user's own lines, removable
AS=$(mktemp -d); export WMAKER_USER_ROOT="$AS/gs"; mkdir -p "$AS/gs/Library/WindowMaker"
printf '#!/bin/sh\npipewire &\nwmvol' > "$AS/gs/Library/WindowMaker/autostart"      # no trailing newline, on purpose
sh tools/wm-autostart install >/dev/null                      || fail "autostart install"
sh tools/wm-autostart install >/dev/null                      || fail "autostart install twice"
A="$AS/gs/Library/WindowMaker/autostart"
[ "$(grep -c '>>> wm-desktop' "$A")" = 1 ]                    || fail "autostart block duplicated"
grep -qx 'wmvol' "$A" && grep -qx 'pipewire &' "$A"           || fail "autostart lost the user's lines"
grep -q 'wmmenud' "$A" && grep -q 'wmnotify' "$A"             || fail "autostart missing our commands"
[ -x "$A" ]                                                   || fail "autostart not executable"
sh -n "$A"                                                    || fail "autostart is not valid sh"
sh tools/wm-autostart install --no-notify >/dev/null; ! grep -q wmnotify "$A" || fail "--no-notify"
sh tools/wm-autostart status >/dev/null                       || fail "autostart status"
sh tools/wm-autostart remove >/dev/null
[ "$(cat "$A")" = "$(printf '#!/bin/sh\npipewire &\nwmvol')" ] || fail "remove did not restore the user's file exactly: $(cat "$A")"
ls "$AS/gs/Library/WindowMaker"/autostart.bak-* >/dev/null    || fail "no backup made"
sh tools/wm-autostart status >/dev/null 2>&1 && fail "status after remove"
rm -rf "$AS"; unset WMAKER_USER_ROOT
