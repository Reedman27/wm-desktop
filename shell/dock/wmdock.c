#define _POSIX_C_SOURCE 200809L
/* wmdock - Dock helper: knows which registry app each running window belongs to.
 *
 *   wmdock list                 running windows: wid, class, app id, icon, title (tab separated)
 *   wmdock watch                same, as a stream of "open"/"close" lines (flushed per event)
 *   wmdock state ID...          WMState "Dock" Applications entries for the given app ids
 *   wmdock attrs                WMWindowAttributes snippet that gives every app its icon
 *
 * Needs only an EWMH window manager (Window Maker provides _NET_CLIENT_LIST). */
#include "../../core/applications/wmapps.h"
#include "../../core/applications/wmicons.h"
#include "../../core/applications/wmmenu.h"
#include "../../core/configuration/wmconfig.h"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { Window w; char instance[128], cls[128], title[256]; } Win;

static Display *dpy;
static int ignore_x_error(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }   /* windows vanish */

static char *prop_utf8(Window w, Atom prop) {
    Atom type, utf8 = XInternAtom(dpy, "UTF8_STRING", False); int fmt; unsigned long n, after; unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, prop, 0, 256, False, utf8, &type, &fmt, &n, &after, &data) != Success || !data) return NULL;
    char *r = strdup((char *)data); XFree(data); return r;
}

static void fill(Win *x, Window w) {
    memset(x, 0, sizeof *x); x->w = w;
    XClassHint ch;
    if (XGetClassHint(dpy, w, &ch)) {
        if (ch.res_name) snprintf(x->instance, sizeof x->instance, "%s", ch.res_name);
        if (ch.res_class) snprintf(x->cls, sizeof x->cls, "%s", ch.res_class);
        XFree(ch.res_name); XFree(ch.res_class);
    }
    char *t = prop_utf8(w, XInternAtom(dpy, "_NET_WM_NAME", False));
    if (!t) { char *n = NULL; if (XFetchName(dpy, w, &n) && n) { t = strdup(n); XFree(n); } }
    if (t) { snprintf(x->title, sizeof x->title, "%s", t); free(t); }
    for (char *p = x->title; *p; p++) if (*p == '\t' || *p == '\n') *p = ' ';
}

static int clients(Win **out) {
    Atom a = XInternAtom(dpy, "_NET_CLIENT_LIST", False), type; int fmt; unsigned long n = 0, after; unsigned char *data = NULL;
    *out = NULL;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), a, 0, 4096, False, XA_WINDOW, &type, &fmt, &n, &after, &data) != Success || !data) return 0;
    Win *v = calloc(n ? n : 1, sizeof *v);
    for (unsigned long i = 0; i < n; i++) fill(&v[i], ((Window *)data)[i]);
    XFree(data); *out = v; return (int)n;
}

/* Window -> registry app: matching lives in WMApps so every component agrees. */
static const WMApp *match(const WMAppRegistry *r, const Win *w) { return wmapps_match_window(r, w->instance, w->cls); }

static void print_win(const char *prefix, const WMAppRegistry *r, const Win *w) {
    const WMApp *a = match(r, w); char icon[4096] = "";
    if (a) wmicons_resolve(a->icon, 64, icon, sizeof icon);
    printf("%s0x%lx\t%s\t%s\t%s\t%s\n", prefix, w->w, w->cls, a ? a->id : "-", icon[0] ? icon : "-", w->title);
}

static void pq(const char *s) { putchar('"'); for (; *s; s++) { if (*s == '"' || *s == '\\') putchar('\\'); putchar(*s); } putchar('"'); }

static char *cap_class(const char *s) {     /* "gnome-calculator" -> "Gnome-calculator": WM's usual instance.Class shape */
    char *r = strdup(s); if (r && r[0] >= 'a' && r[0] <= 'z') r[0] -= 32; return r;
}

int main(int argc, char **argv) {
    if (argc < 2) { fputs("usage: wmdock list|watch|state ID...|attrs\n", stderr); return 2; }
    WMAppRegistry r; wmapps_scan(&r, NULL, getenv("WMDOCK_APPDIR"));
    const char *mode = argv[1];

    if (!strcmp(mode, "state") || !strcmp(mode, "attrs")) {          /* no X connection needed */
        int st = !strcmp(mode, "state"), pos = 0;
        if (st) puts("// Paste into the \"Dock\" dictionary of ~/GNUstep/Defaults/WMState (Window Maker rewrites it on exit)\nApplications = (");
        else puts("{");
        for (int i = st ? 2 : 0; st ? i < argc : (size_t)i < r.count; i++) {
            const WMApp *a = st ? wmapps_find(&r, argv[i]) : &r.apps[i];
            if (!a) { fprintf(stderr, "wmdock: no such app: %s\n", argv[i]); continue; }
            char icon[4096] = "", b[256], name[600];
            int k = wmicons_resolve(a->icon, 64, icon, sizeof icon);
            const char *inst = a->wm_class[0] ? a->wm_class : wmapps_bin(a, b, sizeof b);
            char *cls = cap_class(a->wm_class[0] ? a->wm_class : inst);
            snprintf(name, sizeof name, "%s.%s", inst, cls); free(cls);
            if (st) {
                printf("  {\n    Name = "); pq(name); char *c0 = wmapps_shell_command(a, NULL, 0, wmconfig_get("terminal", "xterm -e")), *c1 = wmmenu_wm_escape(c0);
                printf(";\n    Command = "); pq(c1); free(c0); free(c1);
                printf(";\n    AutoLaunch = No;\n    Lock = No;\n    Forced = No;\n    BuggyApplication = No;\n    Position = \"0,%d\";\n  }%s\n",
                       ++pos, i + 1 < argc ? "," : "");
            } else if (k == 1) { printf("  "); pq(name); printf(" = { Icon = "); pq(icon); puts("; };"); }
        }
        puts(st ? ");" : "}");
        wmapps_free(&r); return 0;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs("wmdock: cannot open display\n", stderr); return 1; }
    XSetErrorHandler(ignore_x_error);
    setvbuf(stdout, NULL, _IOLBF, 0);
    Win *cur; int n = clients(&cur);
    if (!strcmp(mode, "list")) {
        for (int i = 0; i < n; i++) print_win("", &r, &cur[i]);
    } else if (!strcmp(mode, "watch")) {
        Window root = DefaultRootWindow(dpy); Atom list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
        XSelectInput(dpy, root, PropertyChangeMask);
        for (int i = 0; i < n; i++) print_win("open\t", &r, &cur[i]);
        for (;;) {
            XEvent ev; XNextEvent(dpy, &ev);
            if (ev.type != PropertyNotify || ev.xproperty.atom != list) continue;
            Win *nw; int nn = clients(&nw);
            for (int i = 0; i < nn; i++) {            /* opened */
                int seen = 0; for (int j = 0; j < n; j++) if (cur[j].w == nw[i].w) seen = 1;
                if (!seen) print_win("open\t", &r, &nw[i]);
            }
            for (int j = 0; j < n; j++) {             /* closed: use the info saved while it was alive */
                int seen = 0; for (int i = 0; i < nn; i++) if (nw[i].w == cur[j].w) seen = 1;
                if (!seen) print_win("close\t", &r, &cur[j]);
            }
            free(cur); cur = nw; n = nn;
        }
    } else { fprintf(stderr, "unknown command %s\n", mode); return 2; }
    free(cur); wmapps_free(&r); XCloseDisplay(dpy);
    return 0;
}
