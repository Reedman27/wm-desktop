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
HDRS  = $(wildcard core/applications/*.h core/configuration/*.h)
X11LIBS = -lX11

# wmnotify needs libdbus headers; build it only where they exist.
DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1 2>/dev/null)
DBUS_LIBS := $(shell pkg-config --libs dbus-1 2>/dev/null)
NOTIFY := $(if $(DBUS_LIBS),build/wmnotify)

BINS = build/wmapps build/wmmenud build/wmgfx build/wmdock build/wmsettings build/wmfiles $(NOTIFY)

all: $(BINS)

build/wmapps: tools/wmapps.c $(APPS) $(MENU) $(CONF) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/wmapps.c $(APPS) $(MENU) $(CONF)
build/wmmenud: tools/wmmenud.c $(APPS) $(MENU) $(WATCH) $(CONF) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -D_GNU_SOURCE -o $@ tools/wmmenud.c $(APPS) $(MENU) $(WATCH) $(CONF)
build/wmdock: shell/dock/wmdock.c $(APPS) $(MENU) $(ICONS) $(CONF) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ shell/dock/wmdock.c $(APPS) $(MENU) $(ICONS) $(CONF) $(X11LIBS)
build/wmsettings: shell/settings/wmsettings.c $(CONF)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ shell/settings/wmsettings.c $(CONF)
build/wmnotify: shell/notify/wmnotify.c themes/nextstep/palette.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) $(LDFLAGS) -o $@ shell/notify/wmnotify.c $(DBUS_LIBS) $(X11LIBS)
build/wmfiles: shell/files/wmfiles.c themes/nextstep/palette.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ shell/files/wmfiles.c $(X11LIBS)
build/wmgfx: backends/x11/wmgfx.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ backends/x11/wmgfx.c
build/fakewm: tests/fakewm.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/fakewm.c $(X11LIBS)
build/test_wmapps: tests/test_wmapps.c $(APPS) $(MENU) $(WATCH) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -D_GNU_SOURCE -o $@ tests/test_wmapps.c $(APPS) $(MENU) $(WATCH)

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BINS) $(DESTDIR)$(PREFIX)/bin/

check: $(BINS) build/fakewm build/test_wmapps
	build/test_wmapps
	sh tests/run.sh
	sh tests/run_x.sh

# Same tests with AddressSanitizer + UBSan (separate build dir contents; run `make clean` after).
sanitize:
	$(MAKE) clean
	$(MAKE) check CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined"

clean:
	rm -rf build
.PHONY: all check clean install sanitize
