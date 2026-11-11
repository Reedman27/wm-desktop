# Contributing

wm-desktop is a set of **add-ons for Window Maker**, not a desktop environment. Changes should keep it that way.

## Scope

- Add-ons that sit next to Window Maker and follow its theme: notifications, a file browser, menus, dock helpers, settings.
- Dockapps are left alone: no replacements for wmvol, wmclock and friends.
- Everything takes colours, textures and fonts from the active Window Maker theme (see `core/theme/`). A new
  window that hard-codes colours will not be merged.
- X11 first. Wayland (wlmaker) support is a later, separate backend.

## Code

- Plain C99, `-Wall -Wextra` clean, no C++. No new hard dependencies beyond libc, Xlib, Xft, fontconfig and libdbus;
  libwraster stays optional (`HAVE_WRASTER`).
- The Makefile must keep honouring `CC`, `CFLAGS`, `LDFLAGS`, `PKG_CONFIG`, `DESTDIR` and `PREFIX`, and the
  `WITH_WRASTER` / `WITH_NOTIFY` / `WITH_FILES` switches (distro packaging relies on them).
- File operations go through `core/files/wmfileops` (never overwrite, never follow symlinks) and must be undoable.
- Settings go through `wmsettings`; a new key needs a validator, a README line and a test.

## Tests

    make check        # unit tests, X tests and GUI tests (needs Xvfb, dbus-daemon, gdbus)
    make sanitize     # the same under AddressSanitizer and UBSan

GUI tests drive the real programs under Xvfb with `tests/xprobe` (find windows, read pixels, send clicks, keys
and drags). A new feature needs a test that fails without it. CI runs both targets plus builds without
libwraster, libdbus and Xft.

Please keep tests hermetic: no reading of the real `$HOME`, no dependence on programs installed on the machine.

## Commits

One logical change per commit, with a message that says why. Licence: MIT; by contributing you agree your
change is released under it.
