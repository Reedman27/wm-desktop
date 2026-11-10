CC ?= cc
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

# Xft (antialiased UTF-8 text) is required by wmnotify and wmfiles, as it is by Window Maker itself.
# libwraster (optional) gives exact gradients and PNG/XPM icons; without it gradients fall back to two colours.
XFT_CFLAGS := $(shell pkg-config --cflags xft 2>/dev/null)
XFT_LIBS := $(shell pkg-config --libs xft 2>/dev/null)
WR_CFLAGS := $(shell pkg-config --cflags wrlib 2>/dev/null)
WR_LIBS := $(shell pkg-config --libs wrlib 2>/dev/null)
ifneq ($(WR_LIBS),)
WR_CFLAGS += -DHAVE_WRASTER
endif
GUI_CFLAGS = $(XFT_CFLAGS) $(WR_CFLAGS)
GUI_LIBS = $(XFT_LIBS) $(WR_LIBS) $(X11LIBS)

# wmnotify needs libdbus headers; build it only where they exist.
DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1 2>/dev/null)
DBUS_LIBS := $(shell pkg-config --libs dbus-1 2>/dev/null)
HAVE_GUI := $(and $(XFT_LIBS),$(DBUS_LIBS))
NOTIFY := $(if $(HAVE_GUI),build/wmnotify build/wmnotify-send)

BINS = build/wmapps build/wmmenud build/wmgfx build/wmdock build/wmsettings $(if $(XFT_LIBS),build/wmfiles) $(NOTIFY)

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
	install -m 755 $(BINS) tools/wm-autostart $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -m 644 data/applications/*.desktop $(DESTDIR)$(PREFIX)/share/applications/
	for d in scalable 48x48 64x64 128x128; do \
	  install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/$$d/apps; \
	  install -m 644 data/icons/hicolor/$$d/apps/* $(DESTDIR)$(PREFIX)/share/icons/hicolor/$$d/apps/; done

check: $(BINS) build/fakewm build/xprobe build/test_wmapps build/test_wmicons build/test_wmtheme build/test_fileops
	build/test_wmapps
	build/test_wmicons
	build/test_wmtheme
	build/test_fileops
	sh tests/run.sh
	sh tests/run_x.sh
	sh tests/run_gui.sh

# Same tests with AddressSanitizer + UBSan (separate build dir contents; run `make clean` after).
sanitize:
	$(MAKE) clean
	$(MAKE) check CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined"

clean:
	rm -rf build
.PHONY: all check clean install sanitize
