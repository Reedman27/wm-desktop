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
 *   Space / Ctrl+click / Shift+click / Ctrl+A  mark several; Delete, Ctrl+C/X apply to all marked, Esc unmarks
 *   Return  open (file) or enter (folder)      o   open with a command you type
 *   Ctrl+Z  undo the last operation (rename, new folder, copy, move, trash - up to 32 steps)
 *   t       switch to the trash and back; in the trash, r restores the selected item to where it came from
 * The columns follow the disk: a change made elsewhere (a download finishing, another program) shows up on its own.
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
#include <poll.h>
#include <signal.h>
#include <sys/inotify.h>
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

typedef struct { char *name; int dir, marked; } Entry;
typedef struct { char path[PATH_MAX]; Entry *e; int n, sel, scroll; } Column;
enum { M_NONE, M_RENAME, M_NEWDIR, M_OPENWITH };

static Display *dpy; static Window win; static GC gc; static WMThemeX th; static XIC xic;
static Column col[MAXCOL]; static int depth, width = 600, height = 360, show_hidden;
static int active;                 /* column that keyboard navigation acts on */
static long long last_click_ms; static int last_col = -1, last_row = -1;
static int mode = M_NONE; static char input[512], msg[512]; static int msg_err;
static char **clip_list; static int nclip, clip_cut;
enum { U_RENAME, U_MKDIR, U_TRASH, U_COPY, U_MOVE };
typedef struct { int type, group; char a[PATH_MAX], b[PATH_MAX]; } Undo;      /* a = where it was, b = where it is now */
static Undo undo[32]; static int nundo, cur_group;      /* one undo step = every item of one operation */
static char home_dir[PATH_MAX]; static int in_trash;
static int ifd = -1, wd[MAXCOL + 1], nwd; static char watched[PATH_MAX * 4];
static Pixmap tex_body, tex_title; static int tex_w, tex_h, tex_dirty = 1;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static int cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b; if (x->dir != y->dir) return y->dir - x->dir; return strcasecmp(x->name, y->name);
}
static void say(int err, const char *fmt, const char *a) { char b[300]; snprintf(b, sizeof b, "%.290s", a ? a : ""); msg_err = err; snprintf(msg, sizeof msg, fmt, b); }

static void push_undo(int type, const char *a, const char *b) {
    if (nundo == 32) { memmove(&undo[0], &undo[1], 31 * sizeof undo[0]); nundo--; }
    undo[nundo].type = type; undo[nundo].group = cur_group; snprintf(undo[nundo].a, PATH_MAX, "%.4000s", a ? a : ""); snprintf(undo[nundo].b, PATH_MAX, "%.4000s", b ? b : ""); nundo++;
}
static void trash_files_dir(char *out, size_t n) {
    const char *xd = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (xd && *xd) snprintf(out, n, "%.4000s/Trash/files", xd); else snprintf(out, n, "%.4000s/.local/share/Trash/files", h ? h : "");
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
        c->e[c->n].name = strdup(e->d_name); c->e[c->n].dir = isdir; c->e[c->n].marked = 0; c->n++;
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
    char sel[MAXCOL][PATH_MAX]; int had[MAXCOL], scr[MAXCOL], nd = depth; char *marks[MAXCOL]; const int nd0 = nd;
    for (int i = 0; i < nd; i++) {                            /* marked names, newline separated, so marks survive a reload */
        size_t need = 1; for (int j = 0; j < col[i].n; j++) if (col[i].e[j].marked) need += strlen(col[i].e[j].name) + 1;
        marks[i] = malloc(need); marks[i][0] = 0;
        for (int j = 0; j < col[i].n; j++) if (col[i].e[j].marked) { strcat(marks[i], col[i].e[j].name); strcat(marks[i], "\n"); }
    }
    for (int i = 0; i < nd; i++) {
        had[i] = col[i].sel >= 0; scr[i] = col[i].scroll;
        snprintf(sel[i], PATH_MAX, "%.4000s", had[i] ? col[i].e[col[i].sel].name : "");
    }
    if (select_name && active >= 0 && active < nd) { snprintf(sel[active], PATH_MAX, "%.4000s", select_name); had[active] = 1; }
    for (int i = 0; i < nd; i++) {
        char p[PATH_MAX]; snprintf(p, sizeof p, "%.4000s", col[i].path);
        if (load(&col[i], p)) { for (int j = i; j < nd; j++) free_col(&col[j]); depth = i ? i : 1; if (!i) load(&col[0], "/"); break; }
        col[i].scroll = scr[i];
        for (int j = 0; j < col[i].n && marks[i][0]; j++) {
            size_t ln = strlen(col[i].e[j].name); const char *m = marks[i];
            for (; (m = strstr(m, col[i].e[j].name)); m += ln) if ((m == marks[i] || m[-1] == '\n') && m[ln] == '\n') { col[i].e[j].marked = 1; break; }
        }
        if (had[i]) for (int j = 0; j < col[i].n; j++) if (!strcmp(col[i].e[j].name, sel[i])) col[i].sel = j;
        if (i + 1 < nd) {                                    /* only columns whose folder is still the selection survive */
            Column *c = &col[i];
            char want[PATH_MAX * 2]; if (c->sel >= 0) entry_path(c, c->sel, want, sizeof want); else want[0] = 0;
            if (!want[0] || !c->e[c->sel].dir || strcmp(want, col[i + 1].path)) { for (int j = i + 1; j < nd; j++) free_col(&col[j]); depth = i + 1; nd = depth; }
        }
    }
    for (int i = 0; i < nd0; i++) free(marks[i]);
    if (active >= depth) active = depth - 1;
}

/* Watch every visible column's folder; re-armed only when the set of folders changes. */
static void rewatch(void) {
    if (ifd < 0) return;
    char now[sizeof watched]; size_t k = 0; now[0] = 0;
    for (int i = 0; i < depth; i++) { int w = snprintf(now + k, sizeof now - k, "%.4000s\n", col[i].path); if (w < 0 || (size_t)w >= sizeof now - k) break; k += (size_t)w; }
    if (!strcmp(now, watched)) return;
    for (int i = 0; i < nwd; i++) inotify_rm_watch(ifd, wd[i]);
    nwd = 0; snprintf(watched, sizeof watched, "%s", now);
    for (int i = 0; i < depth && nwd < MAXCOL; i++) {
        int w = inotify_add_watch(ifd, col[i].path, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB);
        if (w >= 0) wd[nwd++] = w;
    }
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
            if (sel || c->e[i].marked) { XSetForeground(dpy, gc, sel && act ? th.hi : th.dark); XFillRectangle(dpy, buf, gc, x + 4, y, (unsigned)cw - 8, (unsigned)ROWH); }
            if (c->e[i].marked) { XSetForeground(dpy, gc, th.text); XFillRectangle(dpy, buf, gc, x + 4, y, 3, (unsigned)ROWH); }    /* marker bar */
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
        char line[640]; snprintf(line, sizeof line, "%s %s", mode == M_RENAME ? "Rename to:" : mode == M_OPENWITH ? "Open with:" : "New folder:", input);
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
static Column *cur(void) { if (active >= depth) active = depth - 1; return &col[active]; }
static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static void free_list(char **l, int n) { for (int i = 0; i < n; i++) free(l[i]); free(l); }

/* What an operation applies to: the marked rows of the active column, else its cursor row. Caller frees. */
static char **gather(int *n) {
    Column *c = cur(); int cap = 8, k = 0; char **l = malloc((size_t)cap * sizeof *l); char p[PATH_MAX * 2];
    for (int pass = 0; pass < 2 && !k; pass++)
        for (int i = 0; i < c->n; i++) {
            if (pass == 0 ? !c->e[i].marked : i != c->sel) continue;
            if (k == cap) { cap *= 2; l = realloc(l, (size_t)cap * sizeof *l); }
            entry_path(c, i, p, sizeof p); l[k++] = strdup(p);
            if (pass == 1) break;
        }
    *n = k; return l;
}
static void describe(char **l, int n, char *out, size_t sz) {
    if (n == 1) snprintf(out, sz, "%.200s", base(l[0])); else snprintf(out, sz, "%d items", n);
}

static void clear_marks(void) { for (int i = 0; i < depth; i++) for (int j = 0; j < col[i].n; j++) col[i].e[j].marked = 0; }

static void open_paths(const char *cmd, char **l, int n) {      /* cmd = a shell command line; the files are appended as "$@" */
    if (!n) return;
    char *script = malloc(strlen(cmd) + 16); sprintf(script, "%s \"$@\"", cmd);
    char **av = malloc((size_t)(n + 5) * sizeof *av); int k = 0;
    av[k++] = "sh"; av[k++] = "-c"; av[k++] = script; av[k++] = "sh"; for (int i = 0; i < n; i++) av[k++] = l[i]; av[k] = NULL;
    if (fork() == 0) { setsid(); execvp("sh", av); _exit(127); }
    free(av); free(script);
}
static void open_default(char **l, int n) {
    const char *op = getenv("WMFILES_OPENER"); if (!op || !*op) op = "xdg-open";
    open_paths(op, l, n);
}

static void click(int x, int y, unsigned state) {
    int cw = width / VISIBLE, k = x / cw, ci = first_visible() + k;
    if (ci >= depth || y < TOP || y >= height - SB) return;
    Column *c = &col[ci]; int row = (y - TOP) / ROWH + c->scroll;
    if (row >= c->n) return;
    msg[0] = 0;
    if (state & (ControlMask | ShiftMask)) {                    /* mark rows; the cursor moves but nothing opens */
        if (ci != active) { clear_marks(); }
        int anchor = c->sel < 0 ? row : c->sel;
        if (state & ShiftMask) { for (int i = 0; i < c->n; i++) c->e[i].marked = i >= (anchor < row ? anchor : row) && i <= (anchor < row ? row : anchor); }
        else c->e[row].marked = !c->e[row].marked;
        c->sel = row; for (int i = ci + 1; i < depth; i++) free_col(&col[i]);
        depth = ci + 1; active = ci; last_col = -1; return;
    }
    long long t = now_ms(); int dbl = ci == last_col && row == last_row && t - last_click_ms < 400;
    last_click_ms = t; last_col = ci; last_row = row;
    if (!c->e[row].marked) clear_marks();
    select_row(ci, row); active = ci;
    if (dbl && !c->e[row].dir) { char p[PATH_MAX * 2]; entry_path(c, row, p, sizeof p); char *one[1] = { p }; open_default(one, 1); }
}

static void do_trash(void) {
    int n; char **l = gather(&n); if (!n) { say(1, "Nothing selected", NULL); free(l); return; }
    int keep = cur()->sel, ok = 0; cur_group++;
    for (int i = 0; i < n; i++) {
        char tp[PATH_MAX * 2];
        if (wmfo_trash_to(l[i], tp, sizeof tp)) { say(1, "Cannot trash: %s", wmfo_error()); break; }
        push_undo(U_TRASH, l[i], tp); ok++;
    }
    if (ok == n) { char w[256]; describe(l, n, w, sizeof w); say(0, "Moved to trash: %s", w); }
    free_list(l, n);
    reload_all(NULL); Column *c = cur();
    if (ok && c->n) { int r = keep < c->n ? keep : c->n - 1; select_row(active, r); keep_visible(&col[active]); }
}

static void do_copy_cut(int cut) {
    int n; char **l = gather(&n); if (!n) { say(1, "Nothing selected", NULL); free(l); return; }
    free_list(clip_list, nclip); clip_list = l; nclip = n; clip_cut = cut;
    char w[256]; describe(l, n, w, sizeof w); say(0, cut ? "Cut: %s" : "Copied to clipboard: %s", w);
}

static void do_paste(void) {
    if (!nclip) { say(1, "Clipboard is empty", NULL); return; }
    char out[PATH_MAX * 2] = ""; int ok = 0; cur_group++;
    for (int i = 0; i < nclip; i++) {
        int r = clip_cut ? wmfo_move(clip_list[i], cur()->path, out, sizeof out) : wmfo_copy(clip_list[i], cur()->path, out, sizeof out);
        if (r) { say(1, clip_cut ? "Cannot move: %s" : "Cannot copy: %s", wmfo_error()); break; }
        push_undo(clip_cut ? U_MOVE : U_COPY, clip_list[i], out); ok++;
    }
    if (ok == nclip) { char w[256]; if (nclip == 1) snprintf(w, sizeof w, "%.200s", base(out)); else snprintf(w, sizeof w, "%d items", nclip); say(0, clip_cut ? "Moved: %s" : "Copied: %s", w); }
    if (clip_cut && ok == nclip) { free_list(clip_list, nclip); clip_list = NULL; nclip = 0; }
    reload_all(out[0] ? base(out) : NULL);
    Column *c = cur(); if (ok && c->sel >= 0) { select_row(active, c->sel); keep_visible(c); }
}

static int selected_path(char *out, size_t n) {
    Column *c = cur(); if (c->sel < 0) { say(1, "Nothing selected", NULL); return 0; }
    entry_path(c, c->sel, out, n); return 1;
}

static void begin_input(int m) {
    if (m == M_RENAME) { Column *c = cur(); if (c->sel < 0) { say(1, "Nothing selected", NULL); return; } snprintf(input, sizeof input, "%s", c->e[c->sel].name); }
    else if (m == M_OPENWITH) { Column *c = cur(); if (c->sel < 0 && !c->n) { say(1, "Nothing selected", NULL); return; } input[0] = 0; }
    else input[0] = 0;
    mode = m; msg[0] = 0;
}

static void commit_input(void) {
    int m = mode; mode = M_NONE; char out[PATH_MAX * 2], p[PATH_MAX * 2];
    if (!input[0]) return;
    if (m == M_OPENWITH) { int n; char **l = gather(&n); if (n) { open_paths(input, l, n); say(0, "Opened with: %s", input); } else say(1, "Nothing selected", NULL); free_list(l, n); return; }
    if (m == M_NEWDIR) {
        if (wmfo_mkdir(cur()->path, input, out, sizeof out)) { say(1, "Cannot create folder: %s", wmfo_error()); return; }
        cur_group++; push_undo(U_MKDIR, NULL, out); say(0, "Created: %s", base(out));
    } else {
        if (!selected_path(p, sizeof p)) return;
        if (wmfo_rename(p, input, out, sizeof out)) { say(1, "Cannot rename: %s", wmfo_error()); return; }
        cur_group++; push_undo(U_RENAME, p, out); say(0, "Renamed to: %s", base(out));
    }
    reload_all(base(out));
    Column *c = cur(); if (c->sel >= 0) { select_row(active, c->sel); keep_visible(c); }
}

static void undo_one(void);
static void do_undo(void) {
    if (!nundo) { say(1, "Nothing to undo", NULL); return; }
    int g = undo[nundo - 1].group, before = nundo;
    while (nundo && undo[nundo - 1].group == g) { int n0 = nundo; undo_one(); if (nundo == n0) break; }     /* stop at the first failure */
    if (nundo == before) return;
}
static void undo_one(void) {
    Undo u = undo[nundo - 1]; char out[PATH_MAX * 2] = ""; int r = 0; const char *what = "";
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", u.a); char *sl = strrchr(dir, '/'); if (sl) *(sl == dir ? sl + 1 : sl) = 0;
    switch (u.type) {
    case U_RENAME: what = "Renamed back"; r = wmfo_rename(u.b, base(u.a), out, sizeof out); break;
    case U_MKDIR:  what = "Removed new folder"; r = rmdir(u.b) ? (snprintf(out, sizeof out, "%s", u.b), -2) : 0; break;
    case U_TRASH:  what = "Restored"; r = wmfo_restore(u.b, out, sizeof out); break;
    case U_COPY:   what = "Removed copy"; r = wmfo_trash(u.b); break;
    case U_MOVE:   what = "Moved back"; r = wmfo_move(u.b, dir, out, sizeof out); break;
    }
    if (r) { if (r == -2) say(1, "Cannot undo, folder is not empty: %s", base(u.b)); else say(1, "Cannot undo: %s", wmfo_error()); return; }
    nundo--; say(0, "Undone - %s", what);
    reload_all(out[0] ? base(out) : NULL);
    Column *c = cur(); if (c->sel >= 0) { select_row(active, c->sel); keep_visible(c); }
}

static void toggle_trash(void) {
    char t[PATH_MAX]; trash_files_dir(t, sizeof t);
    const char *dest = in_trash ? home_dir : t;
    for (int i = 0; i < depth; i++) free_col(&col[i]);
    if (load(&col[0], dest)) { say(1, "Cannot open: %s", dest); load(&col[0], home_dir); in_trash = 0; } else in_trash = !in_trash;
    depth = 1; active = 0; last_col = -1;
    say(0, in_trash ? "Trash - r restores the selected item, Delete is not offered here" : "Back to %s", in_trash ? NULL : home_dir);
}

static void restore_selected(void) {
    char p[PATH_MAX * 2], out[PATH_MAX * 2];
    if (!in_trash || active != 0) { say(1, "Restore works on items directly inside the trash (press t)", NULL); return; }
    if (!selected_path(p, sizeof p)) return;
    if (wmfo_restore(p, out, sizeof out)) { say(1, "Cannot restore: %s", wmfo_error()); return; }
    say(0, "Restored to: %s", out); reload_all(NULL);
    Column *c = cur(); if (c->n) { int r = c->sel < c->n ? (c->sel < 0 ? 0 : c->sel) : c->n - 1; select_row(0, r); }
}

static void input_key(KeySym ks, const char *buf, int n) {
    if (ks == XK_Escape) { mode = M_NONE; return; }
    if (ks == XK_Return || ks == XK_KP_Enter) { commit_input(); return; }
    size_t len = strlen(input);
    if (ks == XK_BackSpace) { if (len) { len--; while (len && ((unsigned char)input[len] & 0xC0) == 0x80) len--; input[len] = 0; } return; }
    for (int i = 0; i < n; i++) if ((unsigned char)buf[i] >= 32 && buf[i] != 127 && (buf[i] != '/' || mode == M_OPENWITH) && len + 1 < sizeof input) input[len++] = buf[i];
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
    Column *c = cur();
    if (ctrl && (ks == XK_c || ks == XK_x)) { do_copy_cut(ks == XK_x); return; }
    if (ctrl && ks == XK_v) { do_paste(); return; }
    if (ctrl && ks == XK_a) { clear_marks(); for (int i = 0; i < c->n; i++) c->e[i].marked = 1; return; }
    if (ks == XK_space) { if (c->sel >= 0) { c->e[c->sel].marked = !c->e[c->sel].marked; if (c->sel + 1 < c->n) { select_row(active, c->sel + 1); keep_visible(&col[active]); } } return; }
    if (ks == XK_Escape) { clear_marks(); return; }
    if (ks == XK_o) { begin_input(M_OPENWITH); return; }
    if (ks == XK_Return || ks == XK_KP_Enter) {
        if (c->sel >= 0 && c->e[c->sel].dir) { if (active + 1 < depth) { active++; if (col[active].sel < 0 && col[active].n) select_row(active, 0); } }
        else { int n; char **l = gather(&n); open_default(l, n); free_list(l, n); }
        return;
    }
    if (ctrl && ks == XK_z) { do_undo(); return; }
    if (ks == XK_t) { toggle_trash(); return; }
    if (ks == XK_r) { restore_selected(); return; }
    if (ks == XK_q) exit(0);
    else if (ks == XK_Delete || ks == XK_KP_Delete) { if (in_trash) say(1, "Already in the trash; use r to restore", NULL); else do_trash(); }
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
    signal(SIGCHLD, SIG_IGN);                                    /* openers are not waited for */
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
    snprintf(home_dir, sizeof home_dir, "%s", start);
    load(&col[0], start); depth = 1;
    ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    long long last_ino = 0; int pending = 0;
    for (;;) {
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            if (XFilterEvent(&ev, None)) continue;
            switch (ev.type) {
            case Expose: if (ev.xexpose.count == 0) redraw(); break;
            case ConfigureNotify: width = ev.xconfigure.width; height = ev.xconfigure.height; redraw(); break;
            case ButtonPress: {
                int cw = width / VISIBLE, ci = first_visible() + ev.xbutton.x / cw;
                if (ev.xbutton.button == 1) click(ev.xbutton.x, ev.xbutton.y, ev.xbutton.state);
                else if ((ev.xbutton.button == 4 || ev.xbutton.button == 5) && ci < depth) {
                    Column *c = &col[ci]; int mx = c->n - ROWS; if (mx < 0) mx = 0;
                    c->scroll += ev.xbutton.button == 4 ? -3 : 3; if (c->scroll < 0) c->scroll = 0; if (c->scroll > mx) c->scroll = mx;
                }
                redraw(); break; }
            case KeyPress: key(&ev.xkey); redraw(); break;
            case ClientMessage: if ((Atom)ev.xclient.data.l[0] == del) return 0; break;
            }
        }
        rewatch();
        struct pollfd p[2] = { { ConnectionNumber(dpy), POLLIN, 0 }, { ifd, POLLIN, 0 } };
        poll(p, ifd >= 0 ? 2 : 1, pending ? 150 : 1000);       /* wakes for X, for the disk, and each second for a theme change */
        if (ifd >= 0 && (p[1].revents & POLLIN)) { char buf[4096]; while (read(ifd, buf, sizeof buf) > 0) {} pending = 1; last_ino = now_ms(); }
        if (pending && now_ms() - last_ino >= 150) { pending = 0; reload_all(NULL); redraw(); }
        else if (wmthemex_changed(&th)) redraw();
    }
}
