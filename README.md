# wm-desktop — modern add-ons for Window Maker

Small programs that run *beside* stock Window Maker (X11) and wlmaker (Wayland) and add
features they lack. Nothing here replaces or patches the window manager, and there is no
session manager: start Window Maker the way you already do.

Plain C99, no autotools, no C++ or Objective-C. Runtime deps: libc, and Xlib for the X11 tools;
`wmnotify` also needs libdbus-1 (it is skipped by `make` when the headers are missing).

    make && make check
    make sanitize                                     # same tests under ASan + UBSan
    make install PREFIX=/usr DESTDIR="$pkgdir"        # honours CC, CFLAGS, LDFLAGS

## WMApps — the shared application registry (core/applications)
Everything else consumes this; nothing else scans application directories.

| Piece | What it does |
|---|---|
| `wmapps.c/.h` | Scans XDG + Flatpak `.desktop` files into one registry; tokenizes `Exec`, expands field codes, launches without a shell |
| `wmmenu.c/.h` | Registry → categorized Window Maker / wlmaker plist menu |
| `wmwatch.c/.h` | Recursive inotify watcher over every scanned `applications/` tree |
| `wmicons.c/.h` | Icon name → PNG/XPM file (Window Maker cannot load SVG) |
| `wmapps` (CLI) | `list`, `json`, `menu x11\|wayland`, `launch ID [FILE\|URL…]`, `exec ID […]`, `skipped` |
| `wmmenud` | Keeps the root menu file current (watch → debounce → atomic rewrite, only if content changed) |

### What WMApps does
- **Identity:** desktop-file IDs per spec (`vendor/tool.desktop` → `vendor-tool`), any nesting depth (capped at 8, which also bounds symlink loops).
- **Precedence:** earlier data dirs win. A `Hidden=true`/`NoDisplay`/filtered file in a higher-priority dir still claims its ID, so users can mask system entries. Within a directory, order is sorted, so results are deterministic.
- **Filtering:** `Type=Application`, `Hidden`, `NoDisplay`, `OnlyShowIn`/`NotShowIn` (against `$XDG_CURRENT_DESKTOP`, default `WindowMaker`), `TryExec` (absolute or via `PATH`).
- **Localization:** `Name`, `GenericName`, `Comment` use the spec order `lang_COUNTRY@MODIFIER`, `lang_COUNTRY`, `lang@MODIFIER`, `lang`.
- **Exec:** string-unescaped, then tokenized with double-quote rules (single quotes are tolerated because real files use them). Field codes `%f %F %u %U %i %c %k %%` are expanded per token; deprecated codes are dropped; files become paths or `file://` URIs as the code requires.
- **No shell:** `wmapps launch` forks twice and `execvp()`s the argv; exec failures (missing binary, bad `Path=`) are reported to the caller. Where Window Maker only accepts a string (menu entries, Dock `Command`), every argv element is shell-quoted, so `Exec` text cannot inject commands.
- **Window Maker escaping:** Window Maker runs menu and Dock commands through `ExpandOptions()`, where `\` is an escape and `%w %W %a %A %s %d` are live options. The X11 output escapes `\` and `%` for that; the Wayland output does not (wlmaker has no such layer). Verified against `src/misc.c` in the Window Maker source, not against a running Window Maker.
- **Flatpak:** origin is set by exports directory or `X-Flatpak=`; `@@`/`@@u` file-forwarding markers are passed through untouched. Both user and system exports are scanned even when `XDG_DATA_DIRS` lacks them.
- **Diagnostics:** `wmapps skipped` lists files that are broken (malformed `Exec`, missing `Name`/`Exec`/`Type`), as opposed to intentionally hidden.
- **Window → app matching** (`wmapps_match_window`) lives here so the Dock and any future component agree.

### What WMApps does NOT do (yet)
- `[Desktop Action …]` groups, `Keywords`, `DBusActivatable`, `StartupNotify` / startup-notification, `SingleMainWindow`.
- `LANGUAGE` (gettext list) is ignored; only `LC_ALL` → `LC_MESSAGES` → `LANG`.
- No duplicate detection beyond the desktop-file ID (a native and a Flatpak build of the same program have different IDs and both appear).
- `Exec` binaries are not checked for existence (only `TryExec` is).
- Directories that do not exist at daemon start are picked up by a 60-second re-arm timer, except when their parent also does not exist.
- Icons: `wmicons` supports hicolor + other installed themes by directory search, not the full `index.theme` algorithm (`Inherits`, per-directory `Type`/`Threshold`).

## Other components (untouched by the WMApps work except where noted)
`wmdock` (now uses WMApps for window matching and escapes Dock `Command`), `wmsettings`,
`wmnotify`, `wmfiles`, `wmgfx`. `wmsession` was removed: starting Window Maker is not this project's job.

## Setting it up (X11)
    printf '"~/.config/wm-desktop/root.menu"\n' > ~/GNUstep/Defaults/WMRootMenu
    wmmenud &                          # from your .xinitrc / autostart, before or after wmaker

Dock: `wmdock state org.gnome.Calculator htop` prints entries for the `Dock` dictionary of
`~/GNUstep/Defaults/WMState` (Window Maker rewrites that file on exit, so edit while it is stopped).
`wmdock attrs` prints a `WMWindowAttributes` snippet giving each app its icon.

## Wayland
`wmmenud -b wayland` writes a menu wlmaker loads as is (`ShellExecute`). wlmaker has no IPC and no
foreign-toplevel-management, so a Dock that can focus or minimize windows needs wlmaker changes first.

## Verification status (honest)
- `make check` passes here: 94 unit checks + shell suites, also under ASan/UBSan. Dock window matching was also run under Xvfb with a fake EWMH window manager.
- **Not tested here:** `wmnotify` (no libdbus headers in this sandbox), the `xdotool`-driven `wmfiles` test, and everything against a *running* Window Maker or wlmaker. Menu syntax was checked by reading their sources.
- `wmdock`'s generated `Name = "instance.Class"` is still a best guess from `StartupWMClass`/`Exec`.
- Hybrid-GPU and NVIDIA settings in `wmgfx` are untested on real hardware.
