#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
/* wmfiles - column file browser (Xlib + Xft) that wears your Window Maker theme.
 *
 *   wmfiles [directory]
 *
 * Click a folder to open it in the next column, double-click a file to open it with $WMFILES_OPENER
 * (default xdg-open). Keys: Up/Down select, Right enter, Left back, h toggles hidden files, q quits.
 * File operations (core/files/wmfileops.h - never overwrite, never follow symlinks):
 *   Delete  move to the XDG trash (restorable)      n   new folder        F2  rename
 *   Ctrl+C / Ctrl+X / Ctrl+V  copy / cut / paste into the active column's folder
 * Names are typed in a bar at the bottom: Return accepts, Escape cancels. Errors show in the same bar.
 * Colours, textures (any gradient) and Xft fonts come from the active Window Maker theme: title bar =
 * MenuTitleBack/Color/Font, body = MenuTextBack/Color/Font, selection = Highlight*. Re-read when it changes. */
#include "../../core/theme/wmthemex.h"
#include "../../core/files/wmfileops.h"
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
#define ROWH (th.font->ascent + th.font->descent + 5)
#define TOP (th.title_font->ascent + th.title_font->descent + 10)
#define SB (th.font->ascent + th.font->descent + 8)                 /* status / input bar */
#define ROWS ((height - TOP - SB - 4) / ROWH)

typedef struct { char *name; int dir; } Entry;
typedef struct { char path[PATH_MAX]; Entry *e; int n, sel, scroll; } Column;
enum { M_NONE, M_RENAME, M_NEWDIR };

static Display *dpy; static Window win; static GC gc; static WMThemeX th; static XIC xic;
static Column col[MAXCOL]; static int depth, width = 600, height = 360, show_hidden;
static int active;                 /* column that keyboard navigation acts on */
static long long last_click_ms; static int last_col = -1, last_row = -1;
static int mode = M_NONE; static char input[512], msg[512]; static int msg_err;
static char clip[PATH_MAX]; static int clip_cut;
static Pixmap tex_body, tex_title; static int tex_w, tex_h, tex_dirty = 1;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static int cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b; if (x->dir != y->dir) return y->dir - x->dir; return strcasecmp(x->name, y->name);
}
static void say(int err, const char *fmt, const char *a) { char b[300]; snprintf(b, sizeof b, "%.290s", a ? a : ""); msg_err = err; snprintf(msg, sizeof msg, fmt, b); }

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

/* Re-read every column (after an operation or toggling hidden files), keeping each selection by name. */
static void reload_all(const char *select_name) {
    char sel[MAXCOL][PATH_MAX]; int had[MAXCOL], scr[MAXCOL], nd = depth;
    for (int i = 0; i < nd; i++) {
        had[i] = col[i].sel >= 0; scr[i] = col[i].scroll;
        snprintf(sel[i], PATH_MAX, "%.4000s", had[i] ? col[i].e[col[i].sel].name : "");
    }
    if (select_name && active < nd) { snprintf(sel[active], PATH_MAX, "%.4000s", select_name); had[active] = 1; }
    for (int i = 0; i < nd; i++) {
        char p[PATH_MAX]; snprintf(p, sizeof p, "%.4000s", col[i].path);
        if (load(&col[i], p)) { for (int j = i; j < nd; j++) free_col(&col[j]); depth = i ? i : 1; if (!i) load(&col[0], "/"); break; }
        col[i].scroll = scr[i];
        if (had[i]) for (int j = 0; j < col[i].n; j++) if (!strcmp(col[i].e[j].name, sel[i])) col[i].sel = j;
        if (i + 1 < nd) {                                    /* only columns whose folder is still the selection survive */
            Column *c = &col[i];
            char want[PATH_MAX * 2]; if (c->sel >= 0) entry_path(c, c->sel, want, sizeof want); else want[0] = 0;
            if (!want[0] || !c->e[c->sel].dir || strcmp(want, col[i + 1].path)) { for (int j = i + 1; j < nd; j++) free_col(&col[j]); depth = i + 1; nd = depth; }
        }
    }
    if (active >= depth) active = depth - 1;
}

static void keep_visible(Column *c) {
    if (c->sel < c->scroll) c->scroll = c->sel;
    else if (c->sel >= c->scroll + ROWS) c->scroll = c->sel - ROWS + 1;
}

static int first_visible(void) { return depth > VISIBLE ? depth - VISIBLE : 0; }

/* ---------- drawing ---------- */
static void textures(void) {                                 /* cached; rebuilt on resize or theme change */
    if (!tex_dirty && tex_w == width && tex_h == height) return;
    if (tex_body) XFreePixmap(dpy, tex_body);
    if (tex_title) XFreePixmap(dpy, tex_title);
    int bar = TOP - 3;
    tex_body = wmthemex_texture(dpy, &th, &th.text_tex, width, height);
    tex_title = wmthemex_texture(dpy, &th, &th.title_tex, width, bar > SB ? bar : SB);
    tex_w = width; tex_h = height; tex_dirty = 0;
}

static void bevel(Pixmap p, int x, int y, int w, int h, int sunken) {
    XSetForeground(dpy, gc, sunken ? th.dark : th.light); XDrawLine(dpy, p, gc, x, y, x + w - 2, y); XDrawLine(dpy, p, gc, x, y, x, y + h - 2);
    XSetForeground(dpy, gc, sunken ? th.light : th.dark); XDrawLine(dpy, p, gc, x + 1, y + h - 1, x + w - 1, y + h - 1); XDrawLine(dpy, p, gc, x + w - 1, y + 1, x + w - 1, y + h - 1);
}

static void text(XftDraw *d, XftColor *c, XftFont *f, int x, int y, const char *s) {
    XftDrawStringUtf8(d, c, f, x, y + f->ascent, (const FcChar8 *)s, (int)strlen(s));
}

/* shorten to maxw pixels with an ellipsis, on a UTF-8 boundary */
static void clipped(XftFont *f, const char *s, int maxw, char *out, size_t n) {
    snprintf(out, n, "%s", s); int len = (int)strlen(out);
    if (wmx_text_width(dpy, f, out, len) <= maxw) return;
    while (len > 0 && wmx_text_width(dpy, f, out, len) > maxw - 10) { len--; while (len && ((unsigned char)out[len] & 0xC0) == 0x80) len--; }
    out[len] = 0; strncat(out, "\xE2\x80\xA6", n - strlen(out) - 1);
}

static void redraw(void) {
    if (wmthemex_changed(&th)) { wmthemex_load(dpy, &th, NULL, NULL); XSetWindowBackground(dpy, win, th.face); tex_dirty = 1; }
    textures();
    int sc = DefaultScreen(dpy);
    Pixmap buf = XCreatePixmap(dpy, win, (unsigned)width, (unsigned)height, (unsigned)DefaultDepth(dpy, sc));
    XftDraw *d = XftDrawCreate(dpy, buf, DefaultVisual(dpy, sc), DefaultColormap(dpy, sc));
    if (tex_body) XCopyArea(dpy, tex_body, buf, gc, 0, 0, (unsigned)width, (unsigned)height, 0, 0);
    else { XSetForeground(dpy, gc, th.face); XFillRectangle(dpy, buf, gc, 0, 0, (unsigned)width, (unsigned)height); }
    int bar = TOP - 3, cw = width / VISIBLE, f = first_visible(), sbt = height - SB;
    /* path bar = a menu title bar */
    if (tex_title) XCopyArea(dpy, tex_title, buf, gc, 0, 0, (unsigned)width, (unsigned)bar, 0, 0);
    XSetForeground(dpy, gc, th.title_dark); XDrawLine(dpy, buf, gc, 0, bar, width - 1, bar);
    char t[PATH_MAX]; clipped(th.title_font, col[active < depth ? active : depth - 1].path, width - 16, t, sizeof t);
    text(d, &th.xtitle_text, th.title_font, 8, 4, t);
    for (int k = 0; k < VISIBLE && f + k < depth; k++) {
        Column *c = &col[f + k]; int x = k * cw;
        XRectangle clip_r = { (short)(x + 4), (short)TOP, (unsigned short)(cw - 8), (unsigned short)(sbt - TOP) };
        XftDrawSetClipRectangles(d, 0, 0, &clip_r, 1);
        for (int r = 0; r < ROWS && r + c->scroll < c->n; r++) {
            int i = r + c->scroll, y = TOP + r * ROWH, sel = i == c->sel, act = f + k == active;
            if (sel) { XSetForeground(dpy, gc, act ? th.hi : th.dark); XFillRectangle(dpy, buf, gc, x + 4, y, (unsigned)cw - 8, (unsigned)ROWH); }
            XftColor *fg = sel && act ? &th.xhi_text : &th.xtext;
            char nm[NAME_MAX * 2 + 4]; clipped(th.font, c->e[i].name, cw - 36, nm, sizeof nm);
            text(d, fg, th.font, x + 8, y + 2, nm);
            if (c->e[i].dir) text(d, fg, th.font, x + cw - 18, y + 2, ">");
        }
        XftDrawSetClip(d, 0);
        bevel(buf, x + 2, TOP - 1, cw - 4, sbt - TOP + 1, 1);
    }
    /* status / input bar */
    if (tex_title) XCopyArea(dpy, tex_title, buf, gc, 0, 0, (unsigned)width, (unsigned)SB, 0, sbt);
    XSetForeground(dpy, gc, th.title_light); XDrawLine(dpy, buf, gc, 0, sbt, width - 1, sbt);
    if (mode != M_NONE) {
        char line[640]; snprintf(line, sizeof line, "%s %s", mode == M_RENAME ? "Rename to:" : "New folder:", input);
        clipped(th.font, line, width - 24, t, sizeof t);
        int w = wmx_text_width(dpy, th.font, t, (int)strlen(t));
        text(d, &th.xtitle_text, th.font, 8, sbt + 4, t);
        XSetForeground(dpy, gc, th.title_text); XFillRectangle(dpy, buf, gc, 10 + w, sbt + 4, 2, (unsigned)(th.font->ascent + th.font->descent));
    } else if (msg[0]) {
        clipped(th.font, msg, width - 16, t, sizeof t);
        text(d, msg_err ? &th.xhi_text : &th.xtitle_text, th.font, 8, sbt + 4, t);
    }
    XCopyArea(dpy, buf, win, gc, 0, 0, (unsigned)width, (unsigned)height, 0, 0);
    XftDrawDestroy(d); XFreePixmap(dpy, buf); XFlush(dpy);
}

/* ---------- actions ---------- */
static void open_file(const char *path) {
    const char *op = getenv("WMFILES_OPENER"); if (!op || !*op) op = "xdg-open";
    if (fork() == 0) { setsid(); execlp(op, op, path, (char *)NULL); _exit(127); }
}

static void click(int x, int y) {
    int cw = width / VISIBLE, k = x / cw, ci = first_visible() + k;
    if (ci >= depth || y < TOP || y >= height - SB) return;
    Column *c = &col[ci]; int row = (y - TOP) / ROWH + c->scroll;
    if (row >= c->n) return;
    long long t = now_ms(); int dbl = ci == last_col && row == last_row && t - last_click_ms < 400;
    last_click_ms = t; last_col = ci; last_row = row;
    select_row(ci, row); active = ci; msg[0] = 0;
    if (dbl && !c->e[row].dir) { char p[PATH_MAX * 2]; entry_path(c, row, p, sizeof p); open_file(p); }
}

static Column *cur(void) { if (active >= depth) active = depth - 1; return &col[active]; }

static int selected_path(char *out, size_t n) {
    Column *c = cur(); if (c->sel < 0) { say(1, "Nothing selected", NULL); return 0; }
    entry_path(c, c->sel, out, n); return 1;
}

static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static void do_trash(void) {
    char p[PATH_MAX * 2]; if (!selected_path(p, sizeof p)) return;
    int keep = cur()->sel;
    if (wmfo_trash(p)) { say(1, "Cannot trash: %s", wmfo_error()); return; }
    say(0, "Moved to trash: %s", base(p));
    reload_all(NULL); Column *c = cur();
    if (c->n) { int r = keep < c->n ? keep : c->n - 1; select_row(active, r); keep_visible(&col[active]); }
}

static void do_paste(void) {
    if (!clip[0]) { say(1, "Clipboard is empty", NULL); return; }
    char out[PATH_MAX * 2];
    int r = clip_cut ? wmfo_move(clip, cur()->path, out, sizeof out) : wmfo_copy(clip, cur()->path, out, sizeof out);
    if (r) { say(1, clip_cut ? "Cannot move: %s" : "Cannot copy: %s", wmfo_error()); return; }
    say(0, clip_cut ? "Moved: %s" : "Copied: %s", base(out)); if (clip_cut) clip[0] = 0;
    reload_all(base(out));
    Column *c = cur(); if (c->sel >= 0) { select_row(active, c->sel); keep_visible(c); }
}

static void begin_input(int m) {
    if (m == M_RENAME) { Column *c = cur(); if (c->sel < 0) { say(1, "Nothing selected", NULL); return; } snprintf(input, sizeof input, "%s", c->e[c->sel].name); }
    else input[0] = 0;
    mode = m; msg[0] = 0;
}

static void commit_input(void) {
    int m = mode; mode = M_NONE; char out[PATH_MAX * 2], p[PATH_MAX * 2];
    if (!input[0]) return;
    if (m == M_NEWDIR) {
        if (wmfo_mkdir(cur()->path, input, out, sizeof out)) { say(1, "Cannot create folder: %s", wmfo_error()); return; }
        say(0, "Created: %s", base(out));
    } else {
        if (!selected_path(p, sizeof p)) return;
        if (wmfo_rename(p, input, out, sizeof out)) { say(1, "Cannot rename: %s", wmfo_error()); return; }
        say(0, "Renamed to: %s", base(out));
    }
    reload_all(base(out));
    Column *c = cur(); if (c->sel >= 0) { select_row(active, c->sel); keep_visible(c); }
}

static void input_key(KeySym ks, const char *buf, int n) {
    if (ks == XK_Escape) { mode = M_NONE; return; }
    if (ks == XK_Return || ks == XK_KP_Enter) { commit_input(); return; }
    size_t len = strlen(input);
    if (ks == XK_BackSpace) { if (len) { len--; while (len && ((unsigned char)input[len] & 0xC0) == 0x80) len--; input[len] = 0; } return; }
    for (int i = 0; i < n; i++) if ((unsigned char)buf[i] >= 32 && buf[i] != 127 && buf[i] != '/' && len + 1 < sizeof input) input[len++] = buf[i];
    input[len] = 0;
}

static void key(XKeyEvent *ke) {
    char buf[64]; KeySym ks = NoSymbol; int n;
    Status st;
    if (xic) { n = Xutf8LookupString(xic, ke, buf, (int)sizeof buf - 1, &ks, &st); if (st == XLookupNone) return; if (st == XLookupKeySym) n = 0; }
    else n = XLookupString(ke, buf, (int)sizeof buf - 1, &ks, NULL);
    if (n < 0) n = 0;
    if (mode != M_NONE) { input_key(ks, buf, n); return; }
    int ctrl = ke->state & ControlMask; msg[0] = 0;
    Column *c = cur(); char p[PATH_MAX * 2];
    if (ctrl && (ks == XK_c || ks == XK_x)) { if (selected_path(p, sizeof p)) { snprintf(clip, sizeof clip, "%.4000s", p); clip_cut = ks == XK_x; say(0, clip_cut ? "Cut: %s" : "Copied to clipboard: %s", base(p)); } return; }
    if (ctrl && ks == XK_v) { do_paste(); return; }
    if (ks == XK_q) exit(0);
    else if (ks == XK_Delete || ks == XK_KP_Delete) do_trash();
    else if (ks == XK_n) begin_input(M_NEWDIR);
    else if (ks == XK_F2) begin_input(M_RENAME);
    else if (ks == XK_h) {
        show_hidden = !show_hidden; reload_all(NULL);
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
    if (wmthemex_load(dpy, &th, NULL, NULL) < 0) { fputs("wmfiles: no usable fonts (is fontconfig set up?)\n", stderr); return 1; }
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 100, 100, (unsigned)width, (unsigned)height, 0, 0, th.face);
    XStoreName(dpy, win, "File Viewer"); XClassHint ch = { "wmfiles", "WMFiles" }; XSetClassHint(dpy, win, &ch);
    Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False); XSetWMProtocols(dpy, win, &del, 1);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask | StructureNotifyMask);
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetLocaleModifiers("");
    XIM xim = XOpenIM(dpy, NULL, NULL, NULL);
    if (xim) xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win, XNFocusWindow, win, NULL);
    XMapWindow(dpy, win);
    load(&col[0], start); depth = 1;
    for (;;) {
        XEvent ev; XNextEvent(dpy, &ev);
        if (XFilterEvent(&ev, None)) continue;
        switch (ev.type) {
        case Expose: if (ev.xexpose.count == 0) redraw(); break;
        case ConfigureNotify: width = ev.xconfigure.width; height = ev.xconfigure.height; redraw(); break;
        case ButtonPress: {
            int cw = width / VISIBLE, ci = first_visible() + ev.xbutton.x / cw;
            if (ev.xbutton.button == 1) click(ev.xbutton.x, ev.xbutton.y);
            else if ((ev.xbutton.button == 4 || ev.xbutton.button == 5) && ci < depth) {
                Column *c = &col[ci]; int mx = c->n - ROWS; if (mx < 0) mx = 0;
                c->scroll += ev.xbutton.button == 4 ? -3 : 3; if (c->scroll < 0) c->scroll = 0; if (c->scroll > mx) c->scroll = mx;
            }
            redraw(); break; }
        case KeyPress: key(&ev.xkey); redraw(); break;
        case ClientMessage: if ((Atom)ev.xclient.data.l[0] == del) return 0; break;
        }
    }
}
