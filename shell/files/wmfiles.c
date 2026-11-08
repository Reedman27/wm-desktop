#define _POSIX_C_SOURCE 200809L
/* wmfiles - NeXTSTEP-style column file browser (Xlib only).
 *
 *   wmfiles [directory]
 *
 * Click a folder to open it in the next column, double-click a file to open it with
 * $WMFILES_OPENER (default xdg-open). Keys: Up/Down select, Right enter, Left back,
 * h toggles hidden files, q quits. Wheel scrolls the column under the pointer. */
#include "../../themes/nextstep/palette.h"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VISIBLE 3
#define MAXCOL 64
#define ROWH 18
#define TOP 24

typedef struct { char *name; int dir; } Entry;
typedef struct { char path[PATH_MAX]; Entry *e; int n, sel, scroll; } Column;

static Display *dpy; static Window win; static GC gc; static XFontStruct *font;
static Column col[MAXCOL]; static int depth, width = 600, height = 360, show_hidden;
static unsigned long c_face, c_dark, c_black, c_white, c_light;
static int active;                 /* column that keyboard navigation acts on */
static long long last_click_ms; static int last_col = -1, last_row = -1;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static unsigned long alloc(unsigned long rgb) {
    XColor c; c.red = ((rgb >> 16) & 255) * 257; c.green = ((rgb >> 8) & 255) * 257; c.blue = (rgb & 255) * 257; c.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), &c); return c.pixel;
}

static int cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b; if (x->dir != y->dir) return y->dir - x->dir; return strcasecmp(x->name, y->name);
}

static void free_col(Column *c) { for (int i = 0; i < c->n; i++) free(c->e[i].name); free(c->e); c->e = NULL; c->n = 0; }

static int load(Column *c, const char *path) {
    DIR *d = opendir(path); if (!d) return -1;
    free_col(c); snprintf(c->path, sizeof c->path, "%s", path); c->sel = -1; c->scroll = 0;
    int cap = 64; c->e = malloc((size_t)cap * sizeof *c->e); struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (!show_hidden && e->d_name[0] == '.')) continue;
        char full[PATH_MAX * 2]; snprintf(full, sizeof full, "%s/%s", strcmp(path, "/") ? path : "", e->d_name);
        struct stat st; int isdir = !stat(full, &st) && S_ISDIR(st.st_mode);
        if (c->n == cap) { cap *= 2; c->e = realloc(c->e, (size_t)cap * sizeof *c->e); }
        c->e[c->n].name = strdup(e->d_name); c->e[c->n].dir = isdir; c->n++;
    }
    closedir(d); qsort(c->e, (size_t)c->n, sizeof *c->e, cmp); return 0;
}

static void entry_path(const Column *c, int i, char *out, size_t n) {
    snprintf(out, n, "%s/%s", strcmp(c->path, "/") ? c->path : "", c->e[i].name);
}

static void select_row(int ci, int row) {
    Column *c = &col[ci]; if (row < 0 || row >= c->n) return;
    c->sel = row;
    for (int i = ci + 1; i < depth; i++) free_col(&col[i]);
    depth = ci + 1;
    if (c->e[row].dir && depth < MAXCOL) { char p[PATH_MAX * 2]; entry_path(c, row, p, sizeof p); if (!load(&col[depth], p)) depth++; }
}

static void bevel(int x, int y, int w, int h, int sunken) {
    XSetForeground(dpy, gc, sunken ? c_dark : c_white); XDrawLine(dpy, win, gc, x, y, x + w - 2, y); XDrawLine(dpy, win, gc, x, y, x, y + h - 2);
    XSetForeground(dpy, gc, sunken ? c_white : c_dark); XDrawLine(dpy, win, gc, x + 1, y + h - 1, x + w - 1, y + h - 1); XDrawLine(dpy, win, gc, x + w - 1, y + 1, x + w - 1, y + h - 1);
}

static int first_visible(void) { return depth > VISIBLE ? depth - VISIBLE : 0; }

static void redraw(void) {
    XSetForeground(dpy, gc, c_face); XFillRectangle(dpy, win, gc, 0, 0, (unsigned)width, (unsigned)height);
    XSetFont(dpy, gc, font->fid);
    const char *cur = col[active < depth ? active : depth - 1].path;                        /* path bar */
    XSetForeground(dpy, gc, c_black); XDrawString(dpy, win, gc, 8, 16, cur, (int)strlen(cur));
    int cw = width / VISIBLE, f = first_visible();
    for (int k = 0; k < VISIBLE && f + k < depth; k++) {
        Column *c = &col[f + k]; int x = k * cw, rows = (height - TOP - 4) / ROWH;
        XSetForeground(dpy, gc, c_white); XFillRectangle(dpy, win, gc, x + 3, TOP, (unsigned)cw - 6, (unsigned)(height - TOP - 3));
        for (int r = 0; r < rows && r + c->scroll < c->n; r++) {
            int i = r + c->scroll, y = TOP + r * ROWH; int sel = i == c->sel;
            if (sel) { XSetForeground(dpy, gc, f + k == active ? c_black : c_light); XFillRectangle(dpy, win, gc, x + 4, y, (unsigned)cw - 8, ROWH); }
            XSetForeground(dpy, gc, sel && f + k == active ? c_white : c_black);
            XDrawString(dpy, win, gc, x + 8, y + 13, c->e[i].name, (int)strlen(c->e[i].name));
            if (c->e[i].dir) XDrawString(dpy, win, gc, x + cw - 16, y + 13, ">", 1);
        }
        bevel(x + 2, TOP - 1, cw - 4, height - TOP, 1);
    }
    XFlush(dpy);
}

static void open_file(const char *path) {
    const char *op = getenv("WMFILES_OPENER"); if (!op || !*op) op = "xdg-open";
    if (fork() == 0) { setsid(); execlp(op, op, path, (char *)NULL); _exit(127); }
}

static void click(int x, int y) {
    int cw = width / VISIBLE, k = x / cw, ci = first_visible() + k;
    if (ci >= depth || y < TOP) return;
    Column *c = &col[ci]; int row = (y - TOP) / ROWH + c->scroll;
    if (row >= c->n) return;
    long long t = now_ms(); int dbl = ci == last_col && row == last_row && t - last_click_ms < 400;
    last_click_ms = t; last_col = ci; last_row = row;
    select_row(ci, row); active = ci;
    if (dbl && !c->e[row].dir) { char p[PATH_MAX * 2]; entry_path(c, row, p, sizeof p); open_file(p); }
}

static void keep_visible(Column *c) {
    int rows = (height - TOP - 4) / ROWH;
    if (c->sel < c->scroll) c->scroll = c->sel;
    else if (c->sel >= c->scroll + rows) c->scroll = c->sel - rows + 1;
}

static void key(KeySym ks) {
    if (active >= depth) active = depth - 1;
    Column *c = &col[active];
    if (ks == XK_q) exit(0);
    else if (ks == XK_h) {                                  /* reload every column, keep selections */
        show_hidden = !show_hidden;
        for (int i = 0; i < depth; i++) {
            char p[PATH_MAX], sel[PATH_MAX] = ""; snprintf(p, sizeof p, "%.4000s", col[i].path);
            if (col[i].sel >= 0) snprintf(sel, sizeof sel, "%.4000s", col[i].e[col[i].sel].name);
            load(&col[i], p);
            for (int j = 0; j < col[i].n; j++) if (!strcmp(col[i].e[j].name, sel)) col[i].sel = j;
        }
    } else if (ks == XK_Down || ks == XK_Up) {
        int r = c->sel < 0 ? 0 : c->sel + (ks == XK_Down ? 1 : -1);
        if (r < 0 || r >= c->n) return;
        select_row(active, r); keep_visible(&col[active]);
    } else if (ks == XK_Right) {
        if (active + 1 < depth) { active++; if (col[active].sel < 0 && col[active].n) select_row(active, 0); }
    } else if (ks == XK_Left && active > 0) active--;
}

int main(int argc, char **argv) {
    char start[PATH_MAX]; const char *arg = argc > 1 ? argv[1] : getenv("HOME");
    if (!arg || !realpath(arg, start)) { fprintf(stderr, "wmfiles: cannot open %s\n", arg ? arg : "(no HOME)"); return 1; }
    dpy = XOpenDisplay(NULL); if (!dpy) { fputs("wmfiles: cannot open display\n", stderr); return 1; }
    int scr = DefaultScreen(dpy);
    font = XLoadQueryFont(dpy, "-*-helvetica-medium-r-normal--12-*-*-*-*-*-iso8859-1"); if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (!font) { fputs("wmfiles: no X fonts\n", stderr); return 1; }
    c_face = alloc(NS_GRAY); c_dark = alloc(NS_DARK_GRAY); c_black = alloc(NS_BLACK); c_white = alloc(NS_WHITE); c_light = alloc(NS_LIGHT_GRAY);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 100, 100, (unsigned)width, (unsigned)height, 0, 0, c_face);
    XStoreName(dpy, win, "File Viewer"); XClassHint ch = { "wmfiles", "WMFiles" }; XSetClassHint(dpy, win, &ch);
    Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False); XSetWMProtocols(dpy, win, &del, 1);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask | StructureNotifyMask);
    gc = XCreateGC(dpy, win, 0, NULL); XMapWindow(dpy, win);
    load(&col[0], start); depth = 1;
    for (;;) {
        XEvent ev; XNextEvent(dpy, &ev);
        switch (ev.type) {
        case Expose: if (ev.xexpose.count == 0) redraw(); break;
        case ConfigureNotify: width = ev.xconfigure.width; height = ev.xconfigure.height; redraw(); break;
        case ButtonPress: {
            int cw = width / VISIBLE, ci = first_visible() + ev.xbutton.x / cw;
            if (ev.xbutton.button == 1) click(ev.xbutton.x, ev.xbutton.y);
            else if ((ev.xbutton.button == 4 || ev.xbutton.button == 5) && ci < depth) {
                Column *c = &col[ci]; int rows = (height - TOP - 4) / ROWH, mx = c->n - rows; if (mx < 0) mx = 0;
                c->scroll += ev.xbutton.button == 4 ? -3 : 3; if (c->scroll < 0) c->scroll = 0; if (c->scroll > mx) c->scroll = mx;
            }
            redraw(); break; }
        case KeyPress: key(XLookupKeysym(&ev.xkey, 0)); redraw(); break;
        case ClientMessage: if ((Atom)ev.xclient.data.l[0] == del) return 0; break;
        }
    }
}
