CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
DESTDIR ?=
CFLAGS ?= -O2 -g
override CFLAGS += -std=c99 -Wall -Wextra

APPS  = core/applications/wmapps.c
MENU  = core/applications/wmmenu.c
WATCH = core/applications/wmwatch.c
ICONS = core/applications/wmicons.c
CONF  = core/configuration/wmconfig.c
THEME = core/theme/wmtheme.c
THEMEX = core/theme/wmthemex.c
HDRS  = $(wildcard core/applications/*.h core/configuration/*.h core/theme/*.h)
FILEOPS = core/files/wmfileops.c
X11LIBS = -lX11

# Optional parts. Each WITH_x is auto (build it when the libraries are found), yes (fail if they are not) or no.
#   WITH_WRASTER  libwraster: exact gradients and PNG/XPM icons (without it gradients fall back to two colours)
#   WITH_NOTIFY   wmnotify and wmnotify-send (need libdbus-1 and Xft)
#   WITH_FILES    wmfiles (needs Xft)
WITH_WRASTER ?= auto
WITH_NOTIFY ?= auto
WITH_FILES ?= auto

# Xft (antialiased UTF-8 text) is required by wmnotify and wmfiles, as it is by Window Maker itself.
XFT_CFLAGS := $(shell $(PKG_CONFIG) --cflags xft fontconfig 2>/dev/null)
XFT_LIBS := $(shell $(PKG_CONFIG) --libs xft fontconfig 2>/dev/null)
ifneq ($(WITH_WRASTER),no)
WR_CFLAGS := $(shell $(PKG_CONFIG) --cflags wrlib 2>/dev/null)
WR_LIBS := $(shell $(PKG_CONFIG) --libs wrlib 2>/dev/null)
endif
ifeq ($(WITH_WRASTER)$(WR_LIBS),yes)
$(error WITH_WRASTER=yes but pkg-config cannot find wrlib)
endif
ifneq ($(WR_LIBS),)
WR_CFLAGS += -DHAVE_WRASTER
endif
GUI_CFLAGS = $(XFT_CFLAGS) $(WR_CFLAGS)
GUI_LIBS = $(XFT_LIBS) $(WR_LIBS) $(X11LIBS)

# wmnotify needs libdbus headers; build it only where they exist.
DBUS_CFLAGS := $(shell $(PKG_CONFIG) --cflags dbus-1 2>/dev/null)
DBUS_LIBS := $(shell $(PKG_CONFIG) --libs dbus-1 2>/dev/null)
ifeq ($(WITH_NOTIFY)$(and $(XFT_LIBS),$(DBUS_LIBS)),yes)
$(error WITH_NOTIFY=yes but dbus-1 and xft were not both found by pkg-config)
endif
ifeq ($(WITH_FILES)$(XFT_LIBS),yes)
$(error WITH_FILES=yes but xft was not found by pkg-config)
endif
HAVE_NOTIFY := $(if $(filter no,$(WITH_NOTIFY)),,$(and $(XFT_LIBS),$(DBUS_LIBS)))
HAVE_FILES := $(if $(filter no,$(WITH_FILES)),,$(XFT_LIBS))
NOTIFY := $(if $(HAVE_NOTIFY),build/wmnotify build/wmnotify-send)

BINS = build/wmapps build/wmmenud build/wmgfx build/wmdock build/wmsettings build/wmtheme $(if $(HAVE_FILES),build/wmfiles) $(NOTIFY)
SCRIPTS = tools/wm-autostart $(if $(HAVE_NOTIFY),tools/wmnotifyctl)

all: $(BINS)

build/wmapps: tools/wmapps.c $(APPS) $(MENU) $(ICONS) $(CONF) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/wmapps.c $(APPS) $(MENU) $(ICONS) $(CONF)
build/wmmenud: tools/wmmenud.c $(APPS) $(MENU) $(WATCH) $(CONF) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -D_GNU_SOURCE -o $@ tools/wmmenud.c $(APPS) $(MENU) $(WATCH) $(CONF)
build/wmdock: shell/dock/wmdock.c $(APPS) $(MENU) $(ICONS) $(CONF) $(THEME) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ shell/dock/wmdock.c $(APPS) $(MENU) $(ICONS) $(CONF) $(THEME) $(X11LIBS)
build/wmtheme: tools/wmtheme.c $(THEME) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/wmtheme.c $(THEME)
build/wmsettings: shell/settings/wmsettings.c $(CONF)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ shell/settings/wmsettings.c $(CONF)
build/wmnotify: shell/notify/wmnotify.c $(THEME) $(THEMEX) $(APPS) $(ICONS) $(CONF) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) $(GUI_CFLAGS) -D_GNU_SOURCE $(LDFLAGS) -o $@ shell/notify/wmnotify.c $(THEME) $(THEMEX) $(APPS) $(ICONS) $(CONF) $(DBUS_LIBS) $(GUI_LIBS)
build/wmnotify-send: shell/notify/wmnotify-send.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) $(LDFLAGS) -o $@ shell/notify/wmnotify-send.c $(DBUS_LIBS)
build/wmfiles: shell/files/wmfiles.c $(THEME) $(THEMEX) $(FILEOPS) $(HDRS) core/files/wmfileops.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(GUI_CFLAGS) -D_GNU_SOURCE $(LDFLAGS) -o $@ shell/files/wmfiles.c $(THEME) $(THEMEX) $(FILEOPS) $(GUI_LIBS)
build/test_fileops: tests/test_fileops.c $(FILEOPS) core/files/wmfileops.h
	@mkdir -p build
	$(CC) $(CFLAGS) -D_GNU_SOURCE $(LDFLAGS) -o $@ tests/test_fileops.c $(FILEOPS)
# same tests with renameat2() disabled, to exercise the link()/rename() fallback used on filesystems without RENAME_NOREPLACE
build/test_fileops_fallback: tests/test_fileops.c $(FILEOPS) core/files/wmfileops.h
	@mkdir -p build
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DWMFO_NO_RENAMEAT2 $(LDFLAGS) -o $@ tests/test_fileops.c $(FILEOPS)
build/wmgfx: backends/x11/wmgfx.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ backends/x11/wmgfx.c
build/fakewm: tests/fakewm.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/fakewm.c $(X11LIBS)
build/xprobe: tests/xprobe.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/xprobe.c $(X11LIBS)
build/test_wmtheme: tests/test_wmtheme.c $(THEME) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_wmtheme.c $(THEME)
build/test_wmicons: tests/test_wmicons.c $(APPS) $(ICONS) $(CONF) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -D_GNU_SOURCE -o $@ tests/test_wmicons.c $(APPS) $(ICONS) $(CONF)
build/test_wmapps: tests/test_wmapps.c $(APPS) $(MENU) $(WATCH) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -D_GNU_SOURCE -o $@ tests/test_wmapps.c $(APPS) $(MENU) $(WATCH)

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BINS) $(SCRIPTS) $(DESTDIR)$(PREFIX)/bin/
ifneq ($(HAVE_FILES),)
	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -m 644 data/applications/*.desktop $(DESTDIR)$(PREFIX)/share/applications/
	for d in scalable 48x48 64x64 128x128; do \
	  install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/$$d/apps; \
	  install -m 644 data/icons/hicolor/$$d/apps/* $(DESTDIR)$(PREFIX)/share/icons/hicolor/$$d/apps/; done
endif
	$(if $(wildcard man/*.1),install -d $(DESTDIR)$(PREFIX)/share/man/man1 && install -m 644 man/*.1 $(DESTDIR)$(PREFIX)/share/man/man1/)

check: $(BINS) build/fakewm build/xprobe build/test_wmapps build/test_wmicons build/test_wmtheme build/test_fileops build/test_fileops_fallback
	build/test_wmapps
	build/test_wmicons
	build/test_wmtheme
	build/test_fileops
	build/test_fileops_fallback
	sh tests/run.sh
	sh tests/run_x.sh
	sh tests/run_gui.sh

# Same tests with AddressSanitizer + UBSan (separate build dir contents; run `make clean` after).
sanitize:
	$(MAKE) clean
	$(MAKE) check CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined"

clean:
	rm -rf build
# Removes what `make install` put there (same PREFIX / DESTDIR). Only files this project installs.
uninstall:
	rm -f $(addprefix $(DESTDIR)$(PREFIX)/bin/,$(notdir $(BINS) $(SCRIPTS)))
	rm -f $(patsubst data/applications/%,$(DESTDIR)$(PREFIX)/share/applications/%,$(wildcard data/applications/*.desktop))
	for d in scalable 48x48 64x64 128x128; do \
	  for f in data/icons/hicolor/$$d/apps/*; do rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/$$d/apps/$$(basename $$f); done; done
	rm -f $(addprefix $(DESTDIR)$(PREFIX)/share/man/man1/,$(notdir $(wildcard man/*.1)))

.PHONY: all check clean install uninstall sanitize
