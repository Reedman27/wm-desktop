#define _POSIX_C_SOURCE 200809L
/* wmnotify - org.freedesktop.Notifications server with NeXTSTEP-styled Xlib popups.
 *
 * Implements Notify, CloseNotification, GetCapabilities, GetServerInformation and the
 * NotificationClosed signal. Popups stack down from the top-right corner; click one to
 * dismiss it. Text is drawn with core X fonts (Latin-1); icons and actions are not shown.
 * Build needs libdbus-1 (every distro ships it: sys-apps/dbus on Gentoo). */
#include "../../themes/nextstep/palette.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dbus/dbus.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXN 16
#define W 320
#define PAD 8
#define GAP 6
#define BUS_NAME "org.freedesktop.Notifications"
#define OBJ_PATH "/org/freedesktop/Notifications"

typedef struct {
    unsigned id; Window win; int h; long long expire;      /* expire: monotonic ms, 0 = never */
    char app[64], summary[256], body[1024];
} Note;

static Display *dpy; static Note notes[MAXN]; static int nn; static unsigned next_id = 1;
static XFontStruct *fbold, *freg; static unsigned long col[5]; static DBusConnection *conn;
static int sw;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

static unsigned long alloc(unsigned long rgb) {
    XColor c; c.red = ((rgb >> 16) & 255) * 257; c.green = ((rgb >> 8) & 255) * 257; c.blue = (rgb & 255) * 257; c.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), &c); return c.pixel;
}

static XFontStruct *load(const char *a, const char *b) {
    XFontStruct *f = XLoadQueryFont(dpy, a); if (!f) f = XLoadQueryFont(dpy, b); if (!f) f = XLoadQueryFont(dpy, "fixed");
    return f;
}

/* Greedy word wrap into at most 'max' lines; returns line count. lines[] point into 'buf'. */
static int wrap(char *buf, int width, char **lines, int max) {
    int n = 0; char *p = buf;
    while (*p && n < max) {
        while (*p == ' ') p++;
        lines[n++] = p; char *last_space = NULL, *q = p;
        for (; *q && *q != '\n'; q++) {
            if (*q == ' ') last_space = q;
            if (XTextWidth(freg, p, (int)(q - p) + 1) > width) { if (last_space) q = last_space; break; }
        }
        if (*q == '\n' || *q == ' ') { *q = 0; p = q + 1; }
        else if (*q) { p = q; }       /* hard break inside a long word */
        else p = q;
        if (*q == 0 && p == q) break;
    }
    return n;
}

static void draw(Note *n) {
    GC gc = XCreateGC(dpy, n->win, 0, NULL);
    XSetForeground(dpy, gc, col[1]); XFillRectangle(dpy, n->win, gc, 0, 0, W, (unsigned)n->h);          /* face */
    XSetForeground(dpy, gc, col[3]); XDrawLine(dpy, n->win, gc, 1, 1, W - 3, 1); XDrawLine(dpy, n->win, gc, 1, 1, 1, n->h - 3);   /* highlight */
    XSetForeground(dpy, gc, col[0]); XDrawLine(dpy, n->win, gc, 2, n->h - 2, W - 2, n->h - 2); XDrawLine(dpy, n->win, gc, W - 2, 2, W - 2, n->h - 2); /* shadow */
    XSetForeground(dpy, gc, col[2]); XDrawRectangle(dpy, n->win, gc, 0, 0, W - 1, (unsigned)n->h - 1);  /* outline */
    int y = PAD + 2 + fbold->ascent;
    XSetFont(dpy, gc, fbold->fid); XDrawString(dpy, n->win, gc, PAD + 2, y, n->summary, (int)strlen(n->summary));
    char buf[1024], *lines[8]; snprintf(buf, sizeof buf, "%s", n->body);
    int k = wrap(buf, W - 2 * PAD - 4, lines, 6);
    XSetFont(dpy, gc, freg->fid);
    for (int i = 0; i < k; i++) { y += freg->ascent + freg->descent + 2; XDrawString(dpy, n->win, gc, PAD + 2, y + (i == 0 ? 4 : 0), lines[i], (int)strlen(lines[i])); }
    XFreeGC(dpy, gc);
}

static int height_for(const Note *n) {
    char buf[1024], *lines[8]; snprintf(buf, sizeof buf, "%s", n->body);
    int k = n->body[0] ? wrap(buf, W - 2 * PAD - 4, lines, 6) : 0;
    int lh = freg->ascent + freg->descent + 2;
    return PAD * 2 + 4 + fbold->ascent + fbold->descent + (k ? k * lh + 4 : 0);
}

static void layout(void) {
    int y = 28;
    for (int i = 0; i < nn; i++) { XMoveWindow(dpy, notes[i].win, sw - W - 12, y); y += notes[i].h + GAP; }
    XFlush(dpy);
}

static void emit_closed(unsigned id, unsigned reason) {
    DBusMessage *s = dbus_message_new_signal(OBJ_PATH, BUS_NAME, "NotificationClosed");
    dbus_message_append_args(s, DBUS_TYPE_UINT32, &id, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, s, NULL); dbus_message_unref(s); dbus_connection_flush(conn);
}

static void close_note(unsigned id, unsigned reason) {
    for (int i = 0; i < nn; i++) if (notes[i].id == id) {
        XDestroyWindow(dpy, notes[i].win); memmove(&notes[i], &notes[i + 1], (size_t)(nn - i - 1) * sizeof *notes); nn--;
        layout(); emit_closed(id, reason); return;
    }
}

static unsigned add_or_replace(unsigned replaces, const char *app, const char *sum, const char *body, int timeout) {
    Note *n = NULL;
    for (int i = 0; i < nn; i++) if (replaces && notes[i].id == replaces) n = &notes[i];
    if (!n) {
        if (nn == MAXN) close_note(notes[0].id, 3);
        n = &notes[nn++]; memset(n, 0, sizeof *n); n->id = next_id++;
        XSetWindowAttributes a; a.override_redirect = True; a.event_mask = ButtonPressMask | ExposureMask;
        a.background_pixel = col[1];
        n->win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, W, 40, 0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWEventMask | CWBackPixel, &a);
        XClassHint ch = { "wmnotify", "WMNotify" }; XSetClassHint(dpy, n->win, &ch);
        XMapRaised(dpy, n->win);
    }
    snprintf(n->app, sizeof n->app, "%s", app); snprintf(n->summary, sizeof n->summary, "%s", sum); snprintf(n->body, sizeof n->body, "%s", body);
    n->h = height_for(n); XResizeWindow(dpy, n->win, W, (unsigned)n->h);
    n->expire = timeout == 0 ? 0 : now_ms() + (timeout < 0 ? 5000 : timeout);
    layout(); draw(n); XFlush(dpy); return n->id;
}

static void reply_caps(DBusMessage *m) {
    DBusMessage *r = dbus_message_new_method_return(m); DBusMessageIter it, arr; const char *c = "body";
    dbus_message_iter_init_append(r, &it); dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
    dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &c); dbus_message_iter_close_container(&it, &arr);
    dbus_connection_send(conn, r, NULL); dbus_message_unref(r);
}

static void handle(DBusMessage *m) {
    const char *member = dbus_message_get_member(m);
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL || !member) return;
    if (!strcmp(member, "GetCapabilities")) reply_caps(m);
    else if (!strcmp(member, "GetServerInformation")) {
        const char *a = "wmnotify", *b = "wm-desktop", *c = "0.1", *d = "1.2";
        DBusMessage *r = dbus_message_new_method_return(m);
        dbus_message_append_args(r, DBUS_TYPE_STRING, &a, DBUS_TYPE_STRING, &b, DBUS_TYPE_STRING, &c, DBUS_TYPE_STRING, &d, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, r, NULL); dbus_message_unref(r);
    } else if (!strcmp(member, "Notify")) {
        DBusMessageIter it; const char *app = "", *icon = "", *sum = "", *body = ""; dbus_uint32_t rep = 0; dbus_int32_t to = -1;
        if (dbus_message_iter_init(m, &it)) {
            dbus_message_iter_get_basic(&it, &app); dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &rep); dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &icon); dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &sum); dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &body); dbus_message_iter_next(&it);
            dbus_message_iter_next(&it);          /* actions */
            dbus_message_iter_next(&it);          /* hints */
            dbus_message_iter_get_basic(&it, &to);
        }
        unsigned id = add_or_replace(rep, app, sum, body, to);
        DBusMessage *r = dbus_message_new_method_return(m);
        dbus_message_append_args(r, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, r, NULL); dbus_message_unref(r);
    } else if (!strcmp(member, "CloseNotification")) {
        dbus_uint32_t id = 0; dbus_message_get_args(m, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
        close_note(id, 3);
        DBusMessage *r = dbus_message_new_method_return(m); dbus_connection_send(conn, r, NULL); dbus_message_unref(r);
    }
    dbus_connection_flush(conn);
}

int main(void) {
    dpy = XOpenDisplay(NULL); if (!dpy) { fputs("wmnotify: cannot open display\n", stderr); return 1; }
    sw = DisplayWidth(dpy, DefaultScreen(dpy));
    fbold = load("-*-helvetica-bold-r-normal--12-*-*-*-*-*-iso8859-1", "-*-*-bold-r-*--13-*-*-*-*-*-iso8859-1");
    freg = load("-*-helvetica-medium-r-normal--12-*-*-*-*-*-iso8859-1", "-*-*-medium-r-*--13-*-*-*-*-*-iso8859-1");
    if (!fbold || !freg) { fputs("wmnotify: no usable X fonts\n", stderr); return 1; }
    col[0] = alloc(NS_DARK_GRAY); col[1] = alloc(NS_GRAY); col[2] = alloc(NS_BLACK); col[3] = alloc(NS_WHITE); col[4] = alloc(NS_LIGHT_GRAY);

    DBusError err; dbus_error_init(&err);
    conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) { fprintf(stderr, "wmnotify: no session bus: %s\n", err.message); return 1; }
    int r = dbus_bus_request_name(conn, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (r != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) { fputs("wmnotify: another notification daemon is running\n", stderr); return 1; }
    int dfd; if (!dbus_connection_get_unix_fd(conn, &dfd)) { fputs("wmnotify: no bus fd\n", stderr); return 1; }

    for (;;) {
        long long next = 0;
        for (int i = 0; i < nn; i++) if (notes[i].expire && (!next || notes[i].expire < next)) next = notes[i].expire;
        int timeout = next ? (int)(next - now_ms()) : -1; if (next && timeout < 0) timeout = 0;
        if (XPending(dpy) == 0 && dbus_connection_get_dispatch_status(conn) != DBUS_DISPATCH_DATA_REMAINS) {
            struct pollfd p[2] = { { dfd, POLLIN, 0 }, { ConnectionNumber(dpy), POLLIN, 0 } };
            poll(p, 2, timeout);
        }
        dbus_connection_read_write(conn, 0);
        DBusMessage *m; while ((m = dbus_connection_pop_message(conn))) { handle(m); dbus_message_unref(m); }
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            for (int i = 0; i < nn; i++) if (notes[i].win == ev.xany.window) {
                if (ev.type == Expose) draw(&notes[i]);
                else if (ev.type == ButtonPress) { close_note(notes[i].id, 2); }
                break;
            }
        }
        long long t = now_ms();
        for (int i = 0; i < nn; ) { if (notes[i].expire && notes[i].expire <= t) close_note(notes[i].id, 1); else i++; }
    }
}
