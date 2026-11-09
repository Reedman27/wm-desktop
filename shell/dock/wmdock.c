#define _POSIX_C_SOURCE 200809L
/* wmdock - Dock helper: knows which registry app each running window belongs to.
 *
 *   wmdock list                 running windows: wid, class, app id, icon, title (tab separated)
 *   wmdock watch                same, as a stream of "open"/"close" lines (flushed per event)
 *   wmdock state ID...          WMState "Dock" Applications entries for the given app ids
 *   wmdock attrs [--always]     WMWindowAttributes snippet that gives every app an icon (theme icon, rendered
 *                               SVG, or a generated tile). Window Maker uses it when the window supplies none;
 *                               --always adds AlwaysUserIcon = Yes so it is used even then.
 *   wmdock apply-icons [--dry-run] [--always]
 *                               merge those icon entries into ~/GNUstep/Defaults/WMWindowAttributes: backup
 *                               first, existing entries never touched, refuses a file it cannot parse.
 *
 * Needs only an EWMH window manager (Window Maker provides _NET_CLIENT_LIST). */
#include "../../core/applications/wmapps.h"
#include "../../core/applications/wmicons.h"
#include "../../core/applications/wmmenu.h"
#include "../../core/configuration/wmconfig.h"
#include "../../core/theme/wmtheme.h"
#include <ctype.h>
#include <time.h>
#include <unistd.h>
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

static int icon_size(void) { int n = atoi(wmconfig_get("icon_size", "64")); return n >= 16 && n <= 256 ? n : 64; }

static void print_win(const char *prefix, const WMAppRegistry *r, const Win *w) {
    const WMApp *a = match(r, w); char icon[4096] = "";
    if (a) wmicons_for_app(a, icon_size(), icon, sizeof icon, NULL);
    printf("%s0x%lx\t%s\t%s\t%s\t%s\n", prefix, w->w, w->cls, a ? a->id : "-", icon[0] ? icon : "-", w->title);
}

static void pq(const char *s) { putchar('"'); for (; *s; s++) { if (*s == '"' || *s == '\\') putchar('\\'); putchar(*s); } putchar('"'); }

static char *cap_class(const char *s) {     /* "gnome-calculator" -> "Gnome-calculator": WM's usual instance.Class shape */
    char *r = strdup(s); if (r && r[0] >= 'a' && r[0] <= 'z') r[0] -= 32; return r;
}

static char *slurp_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    size_t cap = 4096, n = 0; char *b = malloc(cap);
    for (size_t k; b && (k = fread(b + n, 1, cap - n - 1, f)) > 0;) { n += k; if (n + 1 >= cap) { cap *= 2; b = realloc(b, cap); } }
    fclose(f); if (!b) return NULL;
    b[n] = 0; return b;
}

static void pqf(FILE *o, const char *s) { fputc('"', o); for (; *s; s++) { if (*s == '"' || *s == '\\') fputc('\\', o); fputc(*s, o); } fputc('"', o); }

/* Window Maker window name "instance.Class" for an app (same guess `state` and `attrs` use). */
static void app_key(const WMApp *a, char *name, size_t n) {
    char b[256]; const char *inst = a->wm_class[0] ? a->wm_class : wmapps_bin(a, b, sizeof b);
    char *cls = cap_class(a->wm_class[0] ? a->wm_class : inst);
    snprintf(name, n, "%s.%s", inst, cls); free(cls);
}

/* Merge an Icon entry for every app into WMWindowAttributes. Existing entries are never touched or
 * replaced; the old file is copied to WMWindowAttributes.bak-<time> first. Returns 0 on success.
 * (Design and file-safety checks adapted from the user's parallel implementation.) */
static int apply_icons(const WMAppRegistry *r, int dry, int always) {
    char path[4096];
    const char *over = getenv("WMDOCK_ATTRS_FILE");
    if (over && *over) snprintf(path, sizeof path, "%s", over);
    else if (!wmtheme_defaults_path("WMWindowAttributes", path, sizeof path)) { fputs("wmdock: no HOME; set WMAKER_USER_ROOT\n", stderr); return 1; }

    char *old = slurp_file(path), *close = NULL; const char *tail = NULL; int blank = 1;
    for (const char *p = old; p && *p; p++) if (!isspace((unsigned char)*p)) { blank = 0; break; }
    if (old && !blank) {                                   /* must look like a plist dict we can append to */
        const char *p = old; while (isspace((unsigned char)*p)) p++;
        close = strrchr(old, '}');
        if (*p != '{' || !close) { fprintf(stderr, "wmdock: %s is not a { ... } dictionary; not touching it\n", path); free(old); return 1; }
        tail = close; while (tail > old && isspace((unsigned char)tail[-1])) tail--;
        if (tail == old || (tail[-1] != ';' && tail[-1] != '{')) {
            fprintf(stderr, "wmdock: %s does not end in an entry I understand; not touching it\n", path); free(old); return 1;
        }
    }
    char *ent = NULL; size_t entlen = 0; FILE *m = open_memstream(&ent, &entlen);
    int added = 0, kept = 0, noicon = 0;
    for (size_t i = 0; i < r->count; i++) {
        const WMApp *a = &r->apps[i]; char name[600], quoted[620], icon[4096];
        app_key(a, name, sizeof name); snprintf(quoted, sizeof quoted, "\"%s\"", name);
        fflush(m);
        if ((old && strstr(old, quoted)) || (ent && strstr(ent, quoted))) { kept++; continue; }
        if (wmicons_for_app(a, icon_size(), icon, sizeof icon, NULL) == WMICON_NONE) { noicon++; continue; }
        fputs("  ", m); pqf(m, name); fputs(" = { Icon = ", m); pqf(m, icon); fputs(always ? "; AlwaysUserIcon = Yes; };\n" : "; };\n", m); added++;
    }
    fclose(m);
    if (noicon) fprintf(stderr, "wmdock: %d app(s) had no icon at all (is the icon cache writable?)\n", noicon);
    if (dry) { fputs(ent ? ent : "", stdout); fprintf(stderr, "wmdock: would add %d, keep %d existing (%s)\n", added, kept, path); free(ent); free(old); return 0; }
    if (!added) { fprintf(stderr, "wmdock: nothing to add (%d already in %s)\n", kept, path); free(ent); free(old); return 0; }

    char bak[4200] = "", tmp[4200]; snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    if (old) {
        char ts[32]; time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); strftime(ts, sizeof ts, "%Y%m%d%H%M%S", &tm);
        snprintf(bak, sizeof bak, "%s.bak-%s", path, ts);
        FILE *b = fopen(bak, "wb");
        if (!b || fputs(old, b) == EOF || fclose(b)) { fprintf(stderr, "wmdock: cannot write backup %s; nothing changed\n", bak); free(ent); free(old); return 1; }
    }
    FILE *o = fopen(tmp, "wb"); int ok = o != NULL;
    if (ok && (!old || blank)) ok = fputs("{\n", o) != EOF && fputs(ent, o) != EOF && fputs("}\n", o) != EOF;
    else if (ok) ok = fwrite(old, 1, (size_t)(tail - old), o) == (size_t)(tail - old) && fputc('\n', o) != EOF && fputs(ent, o) != EOF && fputs(close, o) != EOF;
    if (!ok || (o && fclose(o)) || rename(tmp, path)) {
        fprintf(stderr, "wmdock: cannot write %s (does its directory exist?); nothing changed\n", path);
        unlink(tmp); free(ent); free(old); return 1;
    }
    fprintf(stderr, "wmdock: added %d icon entries, kept %d you already had -> %s%s%s\nRestart Window Maker to see them.\n",
            added, kept, path, bak[0] ? "\n  backup: " : "", bak);
    free(ent); free(old); return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fputs("usage: wmdock list|watch|state ID...|attrs [--always]|apply-icons [--dry-run] [--always]\n", stderr); return 2; }
    wmicons_set_extra_dir(getenv("WMDOCK_APPDIR"));
    WMAppRegistry r; wmapps_scan(&r, NULL, getenv("WMDOCK_APPDIR"));
    const char *mode = argv[1];

    if (!strcmp(mode, "apply-icons")) {
        int dry = 0, always = 0;
        for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--dry-run")) dry = 1; else if (!strcmp(argv[i], "--always")) always = 1; }
        int rc = apply_icons(&r, dry, always); wmapps_free(&r); return rc;
    }

    if (!strcmp(mode, "state") || !strcmp(mode, "attrs")) {          /* no X connection needed */
        int st = !strcmp(mode, "state"), pos = 0, always = 0, printed = 0;
        for (int i = 2; !st && i < argc; i++) if (!strcmp(argv[i], "--always")) always = 1;
        if (st) puts("// Paste into the \"Dock\" dictionary of ~/GNUstep/Defaults/WMState (Window Maker rewrites it on exit)\nApplications = (");
        else puts("{");
        for (int i = st ? 2 : 0; st ? i < argc : (size_t)i < r.count; i++) {
            const WMApp *a = st ? wmapps_find(&r, argv[i]) : &r.apps[i];
            if (!a) { fprintf(stderr, "wmdock: no such app: %s\n", argv[i]); continue; }
            char icon[4096] = "", b[256], name[600];
            WMIconSource k = st ? WMICON_NONE : wmicons_for_app(a, icon_size(), icon, sizeof icon, NULL);
            const char *inst = a->wm_class[0] ? a->wm_class : wmapps_bin(a, b, sizeof b);
            char *cls = cap_class(a->wm_class[0] ? a->wm_class : inst);
            snprintf(name, sizeof name, "%s.%s", inst, cls); free(cls);
            if (st) {
                printf("%s  {\n    Name = ", printed++ ? ",\n" : ""); pq(name); char *c0 = wmapps_shell_command(a, NULL, 0, wmconfig_get("terminal", "xterm -e")), *c1 = wmmenu_wm_escape(c0);
                printf(";\n    Command = "); pq(c1); free(c0); free(c1);
                printf(";\n    AutoLaunch = No;\n    Lock = No;\n    Forced = No;\n    BuggyApplication = No;\n    Position = \"0,%d\";\n  }",
                       ++pos);
            } else if (k != WMICON_NONE) { printf("  "); pq(name); printf(" = { Icon = "); pq(icon); printf(";%s };\n", always ? " AlwaysUserIcon = Yes;" : ""); }
        }
        puts(st ? "\n);" : "}");
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
