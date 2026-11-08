# wm-desktop — modern add-ons for Window Maker

Small programs that run *beside* stock Window Maker and add features it lacks: an app menu that
knows Flatpak, safe launching, notifications, a file browser, tear-free graphics setup. They never
patch or replace Window Maker, and there is no session manager: start Window Maker the way you do now.

**Scope right now: Window Maker on X11 only.** Wayland/wlmaker and NEXTSPACE are set aside (the
Wayland menu backend is in git history, commit 1). Window Maker's own source is the reference.

Plain C99, no autotools. Needs libc and Xlib; `wmnotify` also needs libdbus-1 (skipped by `make` when
its headers are missing). No systemd, no Flatpak requirement, no GNOME/KDE libraries.

## Build
    make                      # builds into ./build
    make check                # unit + shell tests (X tests need Xvfb, xdotool, dbus-run-session)
    make sanitize             # same tests under ASan + UBSan
    make install PREFIX=/usr/local      # honours CC, CFLAGS, LDFLAGS, DESTDIR

Nothing is installed or written outside the project unless you run `make install` or the tools below.

## Using it

### 1. wmmenud — a root menu that follows your installed apps  (start here)
    wmmenud -o ~/.config/wm-desktop/root.menu &       # -o is the default path shown
Point Window Maker at the file once. Window Maker reads the menu location from its own `WMRootMenu`
defaults file, which holds just the path (`~` is expanded):

    printf '"~/.config/wm-desktop/root.menu"\n' > ~/GNUstep/Defaults/WMRootMenu

Then add `wmmenud &` to `~/GNUstep/Library/WindowMaker/autostart` (Window Maker runs that script at startup). Install or remove a program (native
or Flatpak) and the menu file is rewritten a moment later; Window Maker reloads it when the file's
mtime changes. Options: `-t "kitty -e"` (terminal for `Terminal=true` apps; default from `wmsettings`),
`-T template` (your own menu with a line `@APPLICATIONS@` where the generated "Applications" submenu goes),
`--dir D` (extra data dir), `--once` (write once and exit; good for cron or a package hook).

### 2. wmapps — the registry from the command line
    wmapps list                                  # category, native/flatpak, name, id
    wmapps json                                  # everything, for scripts
    wmapps menu -o apps.menu                     # just the Applications submenu
    wmapps launch org.gnome.Calculator           # start an app, no shell involved
    wmapps launch org.gnome.gedit ~/notes.txt    # with files (paths or URLs)
    wmapps exec htop -t "kitty -e"               # show the argv launch would run, one per line
    wmapps skipped                               # .desktop files that are broken, with the reason
`launch` takes an app ID (or a `StartupWMClass`). Use it for keybindings in WPrefs, e.g. `wmapps launch firefox`.

### 3. wmdock — help for Window Maker's own Dock
    wmdock list                       # running windows -> which app they belong to (tab separated)
    wmdock watch                      # same, as a live stream of open/close lines
    wmdock state firefox htop         # Dock entries to paste into ~/GNUstep/Defaults/WMState
    wmdock attrs                      # WMWindowAttributes snippet giving each app its icon
Window Maker rewrites `WMState` when it exits, so stop it (or edit before login) when pasting. The
generated `Name = "instance.Class"` is a best guess from `StartupWMClass`/`Exec`; if an icon docks but does
not match its window, run the app, check `wmdock list`, and correct the name by hand. Apps whose only icon
is SVG get no icon entry (Window Maker cannot load SVG): `wmdock attrs` skips them.

### 4. wmsettings — a few Window Maker options in one file
    wmsettings list                         # key, value, allowed values
    wmsettings set focus sloppy             # focus | placement | icon_size | animations | dock | terminal
    wmsettings apply [--dry-run]            # writes them into Window Maker via its own `wdwrite`
`apply` needs Window Maker's `wdwrite` in your PATH; restart Window Maker (or use WPrefs' Reload) to see
changes. Six keys only; WPrefs remains the full configuration tool.

### 5. wmnotify — desktop notifications
    wmnotify &            # needs a D-Bus session bus (dbus-launch or your login session provides one)
    notify-send "Hello" "from wmnotify"
Implements the freedesktop notification service: text pop-ups stacked in the top-right, click to dismiss.
Not shown: icons, action buttons. Text is Latin-1 only. Only one notification daemon can own the bus name.

### 6. wmfiles — column file browser
    wmfiles [directory]
Click a folder to open its next column; double-click a file to open it with `xdg-open` (or `$WMFILES_OPENER`).
Keys: Up/Down select, Right enter, Left back, `h` hidden files, `q` quit; wheel scrolls. Browsing only: no
copy, move, delete or rename yet.

### 7. wmgfx — tear-free X11 setup
    wmgfx detect                 # GPU drivers found in /sys/class/drm and the chosen primary
    wmgfx picom                  # print a picom.conf (glx + vsync) tuned for that driver
    wmgfx apply                  # write ~/.config/picom/picom.conf
    wmgfx apply --system         # also write /etc/X11/xorg.conf.d/20-wm-desktop-gpu.conf (run as root)
    wmgfx autostart              # prints: picom --daemon  (put it in your autostart)
Needs picom installed separately. Window Maker has no compositor of its own; this makes picom's vsync work with it.

## Internals

### WMApps — the shared application registry (core/applications)
Everything else consumes this; nothing else scans application directories.

| Piece | What it does |
|---|---|
| `wmapps.c/.h` | Scans XDG + Flatpak `.desktop` files; tokenizes `Exec`, expands field codes, launches without a shell |
| `wmmenu.c/.h` | Registry → categorized Window Maker plist menu |
| `wmwatch.c/.h` | Recursive inotify watcher over every scanned `applications/` tree |
| `wmicons.c/.h` | Icon name → PNG/XPM file Window Maker can load |
| `core/configuration/wmconfig.c` | `~/.config/wm-desktop/settings.conf` (`key = value`) |

- **Identity:** desktop-file IDs per spec (`vendor/tool.desktop` → `vendor-tool`), any nesting depth (capped at 8, also bounding symlink loops).
- **Precedence:** earlier data dirs win. A `Hidden`/`NoDisplay`/filtered file in a higher dir still claims its ID, so users can mask system entries. Sorted scan order keeps results deterministic.
- **Filtering:** `Type=Application`, `Hidden`, `NoDisplay`, `OnlyShowIn`/`NotShowIn` (against `$XDG_CURRENT_DESKTOP`, default `WindowMaker`), `TryExec`.
- **Localization:** `Name`, `GenericName`, `Comment` in spec order `lang_COUNTRY@MODIFIER`, `lang_COUNTRY`, `lang@MODIFIER`, `lang`.
- **Exec:** string-unescaped, tokenized with double-quote rules (single quotes tolerated). Field codes `%f %F %u %U %i %c %k %%` expand per token; deprecated codes are dropped; files become paths or `file://` URIs as each code requires.
- **No shell:** `launch` double-forks and `execvp()`s the argv and reports exec failures. Where Window Maker only accepts a string (menu entries, Dock `Command`), every argv element is shell-quoted, so `Exec` text cannot inject commands.
- **Window Maker escaping:** menu and Dock commands pass through `ExpandOptions()` (`src/misc.c`), where `\` is an escape and `%w %W %a %A %s %d` are live options; WMMenu escapes `\` and `%` accordingly. Checked by reading the source, not by running Window Maker.
- **Flatpak:** origin by exports directory or `X-Flatpak=`; `@@`/`@@u` file-forwarding markers are passed through; user and system exports are scanned even when `XDG_DATA_DIRS` lacks them.
- **Window → app matching** (`wmapps_match_window`) lives here so the Dock helper and future components agree.

### Not implemented in WMApps (yet)
- `[Desktop Action …]` groups, `Keywords`, `DBusActivatable`, `StartupNotify`/startup-notification.
- `LANGUAGE` (gettext list); only `LC_ALL` → `LC_MESSAGES` → `LANG`.
- Duplicate detection beyond the desktop-file ID (a native and a Flatpak build have different IDs and both appear).
- Existence check of the `Exec` binary (only `TryExec` is checked).
- Data dirs that do not exist at daemon start are found by a 60-second timer, unless their parent is missing too.
- Icons: directory search over installed themes, not the full `index.theme` algorithm (`Inherits`, `Threshold`).

## Verification status (honest)
- `make check` passes: 93 unit checks plus shell suites, also under ASan/UBSan. Dock window matching was run under Xvfb against a fake EWMH window manager (`tests/fakewm.c`).
- **Not tested:** `wmnotify` (no libdbus headers in the build sandbox), the `xdotool`-driven `wmfiles` test, `wmgfx` on real hardware (only fake `/sys` data), and every tool against a *running* Window Maker. Menu syntax and escaping were checked against the Window Maker source.
- `themes/nextstep/palette.h` still styles `wmnotify` and `wmfiles` (purely cosmetic leftover; easy to drop).
- No license file yet: choose one before publishing. Reference projects have their own terms (Window Maker is GPL-2.0-or-later).
