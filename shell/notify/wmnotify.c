#define _POSIX_C_SOURCE 200809L
/* wmnotify - org.freedesktop.Notifications server whose popups look like Window Maker menus.
 *
 * Implements Notify, CloseNotification, GetCapabilities, GetServerInformation and the
 * NotificationClosed signal. Popups stack down from the top-right corner; click one to
 * dismiss it. Colours and fonts come from the active Window Maker theme (MenuTitleBack/Color for the
 * summary bar, MenuTextBack/Color for the body; see core/theme/wmtheme.h) and are re-read whenever the
 * theme file changed since the last popup. Urgency 'critical' popups stay until clicked.
 * Text is drawn with core X fonts (Latin-1); icons and actions are not shown.
 * Build needs libdbus-1 (every distro ships it: sys-apps/dbus on Gentoo). */
#include "../../core/theme/wmthemex.h"
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
static WMThemeX th; static DBusConnection *conn;
static int sw;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* Greedy word wrap into at most 'max' lines; returns line count. lines[] point into 'buf'. */
static int wrap(char *buf, int width, char **lines, int max) {
    int n = 0; char *p = buf;
    while (*p && n < max) {
        while (*p == ' ') p++;
        lines[n++] = p; char *last_space = NULL, *q = p;
        for (; *q && *q != '\n'; q++) {
            if (*q == ' ') last_space = q;
            if (XTextWidth(th.font, p, (int)(q - p) + 1) > width) { if (last_space) q = last_space; break; }
        }
        if (*q == '\n' || *q == ' ') { *q = 0; p = q + 1; }
        else if (*q) { p = q; }       /* hard break inside a long word */
        else p = q;
        if (*q == 0 && p == q) break;
    }
    return n;
}

static int title_h(void) { return th.title_font->ascent + th.title_font->descent + 6; }
static int line_h(void) { return th.font->ascent + th.font->descent + 2; }

static void draw(Note *n) {
    GC gc = XCreateGC(dpy, n->win, 0, NULL);
    int th_h = title_h();
    XSetForeground(dpy, gc, th.face); XFillRectangle(dpy, n->win, gc, 0, 0, W, (unsigned)n->h);                  /* body */
    XSetForeground(dpy, gc, th.title_face); XFillRectangle(dpy, n->win, gc, 1, 1, W - 2, (unsigned)th_h);         /* title bar */
    XSetForeground(dpy, gc, th.title_light); XDrawLine(dpy, n->win, gc, 1, 1, W - 3, 1);
    XSetForeground(dpy, gc, th.title_dark); XDrawLine(dpy, n->win, gc, 1, th_h, W - 2, th_h);
    XSetForeground(dpy, gc, th.light); XDrawLine(dpy, n->win, gc, 1, th_h + 1, 1, n->h - 3); XDrawLine(dpy, n->win, gc, 1, th_h + 1, W - 3, th_h + 1);
    XSetForeground(dpy, gc, th.dark); XDrawLine(dpy, n->win, gc, 2, n->h - 2, W - 2, n->h - 2); XDrawLine(dpy, n->win, gc, W - 2, th_h + 1, W - 2, n->h - 2);
    XSetForeground(dpy, gc, th.border); XDrawRectangle(dpy, n->win, gc, 0, 0, W - 1, (unsigned)n->h - 1);          /* outline */
    XSetFont(dpy, gc, th.title_font->fid); XSetForeground(dpy, gc, th.title_text);
    XDrawString(dpy, n->win, gc, PAD, 1 + 3 + th.title_font->ascent, n->summary, (int)strlen(n->summary));
    char buf[1024], *lines[8]; snprintf(buf, sizeof buf, "%s", n->body);
    int k = wrap(buf, W - 2 * PAD, lines, 6);
    XSetFont(dpy, gc, th.font->fid); XSetForeground(dpy, gc, th.text);
    for (int i = 0; i < k; i++) XDrawString(dpy, n->win, gc, PAD, th_h + 1 + PAD / 2 + th.font->ascent + i * line_h(), lines[i], (int)strlen(lines[i]));
    XFreeGC(dpy, gc);
}

static int height_for(const Note *n) {
    char buf[1024], *lines[8]; snprintf(buf, sizeof buf, "%s", n->body);
    int k = n->body[0] ? wrap(buf, W - 2 * PAD, lines, 6) : 0;
    return 1 + title_h() + 2 + (k ? k * line_h() + PAD : 0);
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

/* timeout: ms, -1 = server default (5 s), 0 = never. urgency 2 (critical) never expires. */
static unsigned add_or_replace(unsigned replaces, const char *app, const char *sum, const char *body, int timeout, int urgency) {
    if (wmthemex_changed(&th)) wmthemex_load(dpy, &th, NULL, NULL);          /* theme switched since last popup */
    Note *n = NULL;
    for (int i = 0; i < nn; i++) if (replaces && notes[i].id == replaces) n = &notes[i];
    if (!n) {
        if (nn == MAXN) close_note(notes[0].id, 3);
        n = &notes[nn++]; memset(n, 0, sizeof *n); n->id = next_id++;
        XSetWindowAttributes a; a.override_redirect = True; a.event_mask = ButtonPressMask | ExposureMask;
        a.background_pixel = th.face;
        n->win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, W, 40, 0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWEventMask | CWBackPixel, &a);
        XClassHint ch = { "wmnotify", "WMNotify" }; XSetClassHint(dpy, n->win, &ch);
        XMapRaised(dpy, n->win);
    }
    snprintf(n->app, sizeof n->app, "%s", app); snprintf(n->summary, sizeof n->summary, "%s", sum); snprintf(n->body, sizeof n->body, "%s", body);
    n->h = height_for(n); XResizeWindow(dpy, n->win, W, (unsigned)n->h);
    n->expire = (timeout == 0 || urgency >= 2) ? 0 : now_ms() + (timeout < 0 ? 5000 : timeout);
    layout(); draw(n); XFlush(dpy); return n->id;
}

static void reply_caps(DBusMessage *m) {
    DBusMessage *r = dbus_message_new_method_return(m); DBusMessageIter it, arr; const char *c = "body";
    dbus_message_iter_init_append(r, &it); dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
    dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &c); dbus_message_iter_close_container(&it, &arr);
    dbus_connection_send(conn, r, NULL); dbus_message_unref(r);
}

static void bad_args(DBusMessage *m) {
    DBusMessage *r = dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS, "unexpected argument types");
    if (r) { dbus_connection_send(conn, r, NULL); dbus_message_unref(r); }
}

/* Reads the a{sv} hints at 'it' and returns the "urgency" byte (0 low, 1 normal, 2 critical); 1 if absent. */
static int hint_urgency(DBusMessageIter *it) {
    int urg = 1;
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY) return urg;
    DBusMessageIter arr; dbus_message_iter_recurse(it, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter ent, var; const char *key = NULL;
        dbus_message_iter_recurse(&arr, &ent);
        if (dbus_message_iter_get_arg_type(&ent) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&ent, &key); dbus_message_iter_next(&ent);
            if (key && !strcmp(key, "urgency") && dbus_message_iter_get_arg_type(&ent) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&ent, &var);
                if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_BYTE) { unsigned char b = 1; dbus_message_iter_get_basic(&var, &b); urg = b; }
            }
        }
        dbus_message_iter_next(&arr);
    }
    return urg;
}

static void handle(DBusMessage *m) {
    const char *member = dbus_message_get_member(m);
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL || !member) return;
    if (!strcmp(member, "Notify") && !dbus_message_has_signature(m, "susssasa{sv}i")) bad_args(m);
    else if (!strcmp(member, "CloseNotification") && !dbus_message_has_signature(m, "u")) bad_args(m);
    else if (!strcmp(member, "GetCapabilities")) reply_caps(m);
    else if (!strcmp(member, "GetServerInformation")) {
        const char *a = "wmnotify", *b = "wm-desktop", *c = "0.1", *d = "1.2";
        DBusMessage *r = dbus_message_new_method_return(m);
        dbus_message_append_args(r, DBUS_TYPE_STRING, &a, DBUS_TYPE_STRING, &b, DBUS_TYPE_STRING, &c, DBUS_TYPE_STRING, &d, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, r, NULL); dbus_message_unref(r);
    } else if (!strcmp(member, "Notify")) {
        DBusMessageIter it; const char *app = "", *icon = "", *sum = "", *body = ""; dbus_uint32_t rep = 0; dbus_int32_t to = -1; int urgency = 1;
        if (dbus_message_iter_init(m, &it)) {
            dbus_message_iter_get_basic(&it, &app); dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &rep); dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &icon); dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &sum); dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &body); dbus_message_iter_next(&it);
            dbus_message_iter_next(&it);          /* actions */
            urgency = hint_urgency(&it);          /* hints (a{sv}) */
            dbus_message_iter_next(&it);
            dbus_message_iter_get_basic(&it, &to);
        }
        unsigned id = add_or_replace(rep, app, sum, body, to, urgency);
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
    int files = wmthemex_load(dpy, &th, NULL, NULL);
    if (files < 0) { fputs("wmnotify: no usable X fonts\n", stderr); return 1; }
    fprintf(stderr, "wmnotify: theme from %d Window Maker defaults file(s)%s\n", files, files ? "" : " (none found: using Window Maker's built-in look)");

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
