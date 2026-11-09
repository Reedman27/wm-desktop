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
    wmapps icons [--size 64]                     # the icon every app gets, and where it came from
`launch` takes an app ID (or a `StartupWMClass`). Use it for keybindings in WPrefs, e.g. `wmapps launch firefox`.

### 3. wmdock — help for Window Maker's own Dock
    wmdock list                       # running windows -> which app they belong to (tab separated)
    wmdock watch                      # same, as a live stream of open/close lines
    wmdock state firefox htop         # Dock entries to paste into ~/GNUstep/Defaults/WMState
    wmdock attrs [--always]           # WMWindowAttributes snippet giving EVERY app an icon (see Icons)
    wmdock apply-icons [--dry-run] [--always]   # merge those entries into WMWindowAttributes for you
Window Maker rewrites `WMState` when it exits, so stop it (or edit before login) when pasting. The
generated `Name = "instance.Class"` is a best guess from `StartupWMClass`/`Exec`; if an icon docks but does
not match its window, run the app, check `wmdock list`, and correct the name by hand.

**Icons.** Window Maker takes an app's own icon (from the window itself) first and only falls back to the
`Icon` that `attrs` assigns; `--always` adds `AlwaysUserIcon = Yes` so the assigned one is used even then
(consistent look). Paste the output into the `WMWindowAttributes` file (`~/GNUstep/Defaults/WMWindowAttributes`),
or let `wmdock apply-icons` do it: it writes a `WMWindowAttributes.bak-<time>` backup first, never touches or
replaces entries you already have, refuses a file it cannot parse, and a second run changes nothing. Try
`--dry-run` first. (The file location follows `$WMAKER_USER_ROOT` like Window Maker itself.) Restart Window Maker afterwards.
`wmapps icons` shows what every app gets:

    file       org.gnome.Calculator            theme icon (PNG/XPM) used as is
    converted  org.inkscape.Inkscape           SVG-only app, rendered to a cached PNG
    generated  some-app                        no usable icon anywhere: coloured tile with the app's initial
and flags icons found under a guessed name. No app is left without one.

### 4. wmsettings — a few Window Maker options in one file
    wmsettings list                         # key, value, allowed values
    wmsettings set focus sloppy             # focus | placement | icon_size | animations | dock | terminal | icon_theme
    wmsettings set icon_theme Papirus       # which icon theme wins (else GTK's theme, then hicolor)
    wmsettings apply [--dry-run]            # writes them into Window Maker via its own `wdwrite`
`apply` needs Window Maker's `wdwrite` in your PATH; restart Window Maker (or use WPrefs' Reload) to see
changes. Seven keys only; WPrefs remains the full configuration tool.

### 5. wmnotify — desktop notifications
    wmnotify &            # needs a D-Bus session bus (dbus-launch or your login session provides one)
    notify-send "Hello" "from wmnotify"
    notify-send -u critical "Disk almost full" "stays until you click it"
Implements the freedesktop notification service (Notify, CloseNotification, replaces-id, expiry timeouts, the
urgency hint). Pop-ups stack in the top-right; click to dismiss. `critical` ones never expire. They are drawn
like a Window Maker menu: a title bar with the summary, the body underneath, bevelled in your theme's colours
(see "Themes" below). Not shown: icons, action buttons. Text is Latin-1 only. Only one notification daemon can
own the bus name. Calls with the wrong argument types get a D-Bus `InvalidArgs` error.

### 6. wmfiles — column file browser
    wmfiles [directory]
Ships `data/applications/wmfiles.desktop` and an icon (SVG + 48/64/128 px PNG), installed by `make install`, so it
appears in the generated menu and Dock like any other app. Click a folder to open its next column; double-click a file to open it with `xdg-open` (or `$WMFILES_OPENER`).
Keys: Up/Down select, Right enter, Left back, `h` hidden files, `q` quit; wheel scrolls. Browsing only: no
copy, move, delete or rename yet. Wears your Window Maker theme (below): path bar = menu title bar, columns =
menu body, selected row = highlight colour.

### Themes — how wmnotify and wmfiles follow Window Maker
Applying a Window Maker theme merges its keys into the `WindowMaker` defaults file, so that file *is* the active
look. `wmnotify` and `wmfiles` read it (`$WMAKER_USER_ROOT/Defaults/WindowMaker`, default
`~/GNUstep/Defaults/WindowMaker`), layered over the global file (`/etc/WindowMaker/WindowMaker` or
`/usr/local/etc/WindowMaker/WindowMaker`, or `$WMTHEME_GLOBAL`), then Window Maker's built-in defaults. Keys used:

| Key | Used for |
|---|---|
| `MenuTitleBack`, `MenuTitleColor`, `MenuTitleFont` | notification summary bar, wmfiles path bar |
| `MenuTextBack`, `MenuTextColor`, `MenuTextFont` | notification body, wmfiles columns |
| `HighlightColor`, `HighlightTextColor` | wmfiles selection |
| `MenuDisabledColor`, `FrameBorderColor` | disabled text, window outline |

Switch themes (setstyle, WPrefs, or editing the file) and the next notification, or the next wmfiles redraw, uses
the new look; already-visible notifications keep the old one. Limits, stated plainly: a texture (gradient,
pixmap) is reduced to its first colour; fonts are loaded as classic X core fonts (no Xft/antialiasing), so a
fontconfig name such as `Sans:bold:pixelsize=12` is mapped to a matching core font and falls back to `fixed`
if the X server has none; bevels are derived from the face colour rather than copied from Window Maker's own
renderer, so it looks like a Window Maker menu, not pixel-identical to one.

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
| `wmicons.c/.h` | Finds an icon for every app in a form Window Maker can load (below) |
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

### WMIcons — how an app gets its icon
1. **Theme lookup** per the Icon Theme Specification: `WMICONS_THEME` env, the `icon_theme` setting, or GTK's `gtk-icon-theme-name` (`~/.config/gtk-3.0/settings.ini`, `~/.gtkrc-2.0`); each followed by its `Inherits` chain; then `hicolor`, any other installed theme, and `pixmaps/`. `index.theme` sizes/types/thresholds are honoured, symbolic (monochrome) and non-application directories are skipped, and a larger icon beats a smaller one (downscaling looks better than upscaling).
2. **SVG:** Window Maker cannot load SVG, so an SVG-only hit is rendered to `$XDG_CACHE_HOME/wm-desktop/icons` with `rsvg-convert`, ImageMagick (`magick`/`convert`) or `inkscape`, whichever is in `PATH`. None is required; re-rendered only when the SVG changes.
3. **Other names:** if `Icon=` is missing or wrong, the app is looked up as `X-Flatpak` ID, desktop-file ID, its last component, `StartupWMClass` and program name.
4. **Last resort:** a generated XPM tile (muted colour from the app ID, the app's initial in a 5x7 bitmap font, transparent rounded corners). Needs no tools; Window Maker loads XPM natively.

### Not implemented in WMApps (yet)
- `[Desktop Action …]` groups, `Keywords`, `DBusActivatable`, `StartupNotify`/startup-notification.
- `LANGUAGE` (gettext list); only `LC_ALL` → `LC_MESSAGES` → `LANG`.
- Duplicate detection beyond the desktop-file ID (a native and a Flatpak build have different IDs and both appear).
- Existence check of the `Exec` binary (only `TryExec` is checked).
- Data dirs that do not exist at daemon start are found by a 60-second timer, unless their parent is missing too.
- Icons: scaled (`@2x`) directories are ignored; the generated tile supports Latin letters and digits only (anything else shows `#`); SVG rendering needs one of the converters above, otherwise such apps get the generated tile.

## Verification status (honest)
- `make check` passes, also under `make sanitize` (ASan + UBSan): 93 WMApps, 35 WMIcons and 58 WMTheme unit checks, shell suites (including `wmdock apply-icons` on a scratch `WMAKER_USER_ROOT`), dock window matching against a fake EWMH window manager, and GUI tests.
- **GUI tests** (`tests/run_gui.sh`) run `wmnotify` and `wmfiles` under Xvfb with a private D-Bus session bus (no window manager), send real notifications through `gdbus`, and read pixels back with `tests/xprobe.c` to prove the title bar, body, selection and text are drawn in the colours of a fixture theme file and that changing the file changes the next draw. They also cover replace-id, close, expiry, critical urgency, click-to-dismiss, the single-instance guard and `wmfiles` double-click opening. They SKIP when Xvfb, dbus-daemon, gdbus or a built `wmnotify` is missing.
- A screenshot of a themed notification and `wmfiles` was looked at (Window Maker's default grey-blue menu colours).
- Generated tiles and the `wmfiles` icon were rendered with ImageMagick/rsvg and looked at; I have **not** seen anything inside a running Window Maker (its XPM/PNG loaders and menu/defaults formats were checked by reading its source).
- **Not tested:** every tool against a *running* Window Maker, `wmgfx` on real hardware (only fake `/sys` data), `wmsettings apply` (needs `wdwrite`), real application notification clients other than `gdbus`, and non-Latin text or Xft fonts (not implemented).
- No license file yet: choose one before publishing. Reference projects have their own terms (Window Maker is GPL-2.0-or-later).
