#define _POSIX_C_SOURCE 200809L
/* wmnotify - org.freedesktop.Notifications server that looks like a Window Maker menu.
 *
 * Every pop-up is drawn from the active Window Maker theme (core/theme/wmtheme.h): title bar = MenuTitleBack
 * (solid or any gradient) with MenuTitleColor/Font, body = MenuTextBack with MenuTextColor/Font, buttons and
 * critical pop-ups = HighlightColor, frame = FrameBorderColor. Text is Xft (antialiased, UTF-8). The theme file
 * is watched, so a theme switch restyles open pop-ups. Supports icons (PNG/XPM through libwraster), action
 * buttons, the "urgency", "value" (progress bar), "image-path" and "desktop-entry" hints, and replacing a
 * notification by id. Markup tags in bodies are stripped, not rendered.
 * Settings (wmsettings): notify_corner, notify_timeout. */
#include "../../core/applications/wmapps.h"
#include "../../core/applications/wmicons.h"
#include "../../core/configuration/wmconfig.h"
#include "../../core/theme/wmthemex.h"
#include <X11/Xutil.h>
#include <dbus/dbus.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAXN 8
#define MAXACT 4
#define MAXLINE 10
#define BUS_NAME "org.freedesktop.Notifications"
#define OBJ_PATH "/org/freedesktop/Notifications"

typedef struct { char key[64], label[96]; int x, y, w, h; } Action;
typedef struct {
    unsigned id; Window win; int w, h; long long expire;
    char app[64], summary[256], body[2048], icon[1024], entry[128];
    int urgency, value, nact, hover;
    Action act[MAXACT];
    Pixmap icon_pm; int icon_w, icon_h;
    char line[MAXLINE][256]; int nline;                 /* wrapped body */
    int title_h, text_x, text_y, bar_y;
} Note;

static Display *dpy;
static WMThemeX th;
static Note notes[MAXN]; static int nn; static unsigned next_id = 1;
static DBusConnection *conn;
static WMAppRegistry reg; static time_t reg_time;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static int x_error(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

/* ---------- theme ---------- */
static int load_theme(void) { return wmthemex_load(dpy, &th, NULL, NULL); }
#define TW(f, s, n) wmx_text_width(dpy, (f), (s), (n))
#define BW 1

/* ---------- text helpers (UTF-8 aware) ---------- */
static int char_len(const char *s) { unsigned char c = (unsigned char)*s; return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

/* Longest prefix of s[0..len) that fits in maxw pixels, cut at a character boundary (at least one char). */
static int fit(XftFont *f, const char *s, int len, int maxw) {
    int i = 0, last = 0;
    while (i < len) {
        int cl = char_len(s + i); if (i + cl > len) cl = len - i;
        if (TW(f, s, i + cl) > maxw) break;
        i += cl; last = i;
    }
    return last ? last : (len ? char_len(s) : 0);
}

static void strip_markup(char *s) {
    char *w = s;
    for (char *r = s; *r;) {
        if (*r == '<') {
            if (!strncasecmp(r, "<br", 3)) *w++ = '\n';
            char *e = strchr(r, '>'); if (!e) { *w++ = *r++; continue; }
            r = e + 1;
        } else if (*r == '&') {
            static const struct { const char *ent; char ch; } E[] = { {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''} };
            int hit = 0;
            for (unsigned i = 0; i < sizeof E / sizeof *E; i++) { size_t n = strlen(E[i].ent); if (!strncmp(r, E[i].ent, n)) { *w++ = E[i].ch; r += n; hit = 1; break; } }
            if (!hit) *w++ = *r++;
        } else *w++ = *r++;
    }
    *w = 0;
}

static void wrap(Note *n, int maxw) {
    n->nline = 0; const char *p = n->body; int more = 0;
    while (*p) {
        if (n->nline == MAXLINE) { more = 1; break; }
        while (*p == ' ') p++;
        const char *nl = strchr(p, '\n'); int seg = nl ? (int)(nl - p) : (int)strlen(p);
        if (seg == 0) { if (!nl) break; n->line[n->nline++][0] = 0; p = nl + 1; continue; }
        int take = fit(th.font, p, seg, maxw);
        if (take < seg) {                                                        /* prefer a word boundary */
            int b = take; while (b > 0 && p[b] != ' ') b--;
            if (b > 0) take = b;
        }
        if (take > 250) take = 250;
        memcpy(n->line[n->nline], p, (size_t)take); n->line[n->nline][take] = 0; n->nline++;
        p += take; if (*p == '\n') p++;
    }
    if (more && n->nline) {                                                      /* ellipsis on the last kept line */
        char *l = n->line[n->nline - 1]; size_t len = strlen(l);
        while (len && TW(th.font, l, (int)len) > maxw - 14) { len--; while (len && ((unsigned char)l[len] & 0xC0) == 0x80) len--; }
        l[len] = 0; strncat(l, "\xE2\x80\xA6", 4);
    }
}

/* ---------- icons ---------- */
static const WMApp *registry_find(const char *key) {
    if (!key || !*key) return NULL;
    if (!reg_time || time(NULL) - reg_time > 60) { wmapps_free(&reg); wmapps_scan(&reg, "WindowMaker", NULL); reg_time = time(NULL); }
    return wmapps_find(&reg, key);
}

static void find_icon(Note *n, const char *icon_hint) {
    char path[4096] = "", tmp[4096]; const char *spec = icon_hint && *icon_hint ? icon_hint : n->icon;
    if (!strncmp(spec, "file://", 7)) spec += 7;
    if (spec[0] == '/') { if (!access(spec, R_OK)) snprintf(path, sizeof path, "%s", spec); }
    else if (spec[0]) {
        int r = wmicons_resolve(spec, 32, tmp, sizeof tmp);
        if (r == 1) snprintf(path, sizeof path, "%s", tmp);
        else if (r == 2 && wmicons_convert_svg(tmp, 32, path, sizeof path)) {}
    }
    if (!path[0]) {                                                              /* the app's own icon, via the registry */
        const WMApp *a = registry_find(n->entry); if (!a) a = registry_find(n->app);
        if (a && wmicons_for_app(a, 32, path, sizeof path, NULL) == WMICON_NONE) path[0] = 0;
    }
    if (path[0]) n->icon_pm = wmthemex_icon(dpy, &th, path, 32, th.face_rgb, &n->icon_w, &n->icon_h);
}

/* ---------- layout and drawing ---------- */
static void layout(Note *n) {
    int bw = BW, W = 340, pad = 8, fh = wmx_font_height(th.font);
    n->title_h = wmx_font_height(th.title_font) + 8;
    int inner = W - 2 * bw, tx = pad + (n->icon_pm ? n->icon_w + pad : 0), maxw = inner - tx - pad;
    wrap(n, maxw);
    int y = bw + n->title_h + pad; n->text_x = bw + tx; n->text_y = y;
    int text_h = n->nline * (fh + 2), icon_h = n->icon_pm ? n->icon_h : 0;
    y += text_h > icon_h ? text_h : icon_h;
    n->bar_y = -1;
    if (n->value >= 0) { y += (text_h ? 6 : 0); n->bar_y = y; y += 10; }
    if (n->nact) {
        y += 8; int x = bw + pad, row_h = fh + 8;
        for (int i = 0; i < n->nact; i++) {
            Action *a = &n->act[i]; a->w = TW(th.font, a->label, (int)strlen(a->label)) + 16; a->h = row_h;
            if (x + a->w > bw + inner - pad && x > bw + pad) { x = bw + pad; y += row_h + 4; }
            a->x = x; a->y = y; x += a->w + 6;
        }
        y += row_h;
    }
    int has_body = n->nline || n->icon_pm || n->value >= 0 || n->nact;
    n->w = W; n->h = has_body ? y + pad + bw : bw + n->title_h + bw;
}

static void draw(Note *n) {
    int bw = BW, inner = n->w - 2 * bw, fh = wmx_font_height(th.font), crit = n->urgency == 2;
    GC gc = XCreateGC(dpy, n->win, 0, NULL);
    int sc = DefaultScreen(dpy); XftDraw *d = XftDrawCreate(dpy, n->win, DefaultVisual(dpy, sc), DefaultColormap(dpy, sc));
    /* title bar and body: the theme's textures (any gradient), critical = the highlight colours */
    if (crit) { XSetForeground(dpy, gc, th.hi); XFillRectangle(dpy, n->win, gc, bw, bw, (unsigned)inner, (unsigned)n->title_h); }
    else { Pixmap pm = wmthemex_texture(dpy, &th, &th.title_tex, inner, n->title_h);
           if (pm) { XCopyArea(dpy, pm, n->win, gc, 0, 0, (unsigned)inner, (unsigned)n->title_h, bw, bw); XFreePixmap(dpy, pm); } }
    int bh = n->h - 2 * bw - n->title_h;
    if (bh > 0) { Pixmap pm = wmthemex_texture(dpy, &th, &th.text_tex, inner, bh);
        if (pm) { XCopyArea(dpy, pm, n->win, gc, 0, 0, (unsigned)inner, (unsigned)bh, bw, bw + n->title_h); XFreePixmap(dpy, pm); } }
    /* bevels derived from the face colours, as Window Maker draws its menus */
    XSetForeground(dpy, gc, th.title_light); XDrawLine(dpy, n->win, gc, bw, bw, n->w - 1 - bw, bw);
    XSetForeground(dpy, gc, th.title_dark);  XDrawLine(dpy, n->win, gc, bw, bw + n->title_h - 1, n->w - 1 - bw, bw + n->title_h - 1);
    if (bh > 0) {
        XSetForeground(dpy, gc, th.light); XDrawLine(dpy, n->win, gc, bw, bw + n->title_h, bw, n->h - 1 - bw);
        XSetForeground(dpy, gc, th.dark);  XDrawLine(dpy, n->win, gc, bw, n->h - 1 - bw, n->w - 1 - bw, n->h - 1 - bw);
        XDrawLine(dpy, n->win, gc, n->w - 1 - bw, bw + n->title_h, n->w - 1 - bw, n->h - 1 - bw);
    }
    /* title: summary, ellipsized */
    char t[300]; snprintf(t, sizeof t, "%s", n->summary[0] ? n->summary : n->app);
    int len = (int)strlen(t), maxw = inner - 16;
    if (TW(th.title_font, t, len) > maxw) { while (len > 0 && TW(th.title_font, t, len) > maxw - 12) { len--; while (len && ((unsigned char)t[len] & 0xC0) == 0x80) len--; } t[len] = 0; strncat(t, "\xE2\x80\xA6", 4); len = (int)strlen(t); }
    XftDrawStringUtf8(d, crit ? &th.xhi_text : &th.xtitle_text, th.title_font, bw + 8, bw + (n->title_h - wmx_font_height(th.title_font)) / 2 + th.title_font->ascent, (const FcChar8 *)t, len);
    if (n->icon_pm) XCopyArea(dpy, n->icon_pm, n->win, gc, 0, 0, (unsigned)n->icon_w, (unsigned)n->icon_h, bw + 8, n->text_y);
    for (int i = 0; i < n->nline; i++)
        XftDrawStringUtf8(d, &th.xtext, th.font, n->text_x, n->text_y + i * (fh + 2) + th.font->ascent, (const FcChar8 *)n->line[i], (int)strlen(n->line[i]));
    if (n->bar_y >= 0) {                                    /* progress: outline + fill in the text colour, always legible */
        int x = bw + 8, w = inner - 16, fill = (w - 4) * n->value / 100;
        XSetForeground(dpy, gc, th.text); XDrawRectangle(dpy, n->win, gc, x, n->bar_y, (unsigned)w - 1, 9);
        if (fill > 0) XFillRectangle(dpy, n->win, gc, x + 2, n->bar_y + 2, (unsigned)fill, 6);
    }
    for (int i = 0; i < n->nact; i++) {
        Action *a = &n->act[i]; int hot = n->hover == i;
        if (hot) { XSetForeground(dpy, gc, th.hi); XFillRectangle(dpy, n->win, gc, a->x, a->y, (unsigned)a->w, (unsigned)a->h); }
        else { XSetForeground(dpy, gc, th.text); XDrawRectangle(dpy, n->win, gc, a->x, a->y, (unsigned)a->w - 1, (unsigned)a->h - 1); }
        XftDrawStringUtf8(d, hot ? &th.xhi_text : &th.xtext, th.font, a->x + 8, a->y + 4 + th.font->ascent, (const FcChar8 *)a->label, (int)strlen(a->label));
    }
    XSetForeground(dpy, gc, th.border);
    for (int i = 0; i < bw; i++) XDrawRectangle(dpy, n->win, gc, i, i, (unsigned)(n->w - 1 - 2 * i), (unsigned)(n->h - 1 - 2 * i));
    XftDrawDestroy(d); XFreeGC(dpy, gc);
}

static void place_all(void) {
    int sw = DisplayWidth(dpy, DefaultScreen(dpy)), sh = DisplayHeight(dpy, DefaultScreen(dpy)), margin = 12;
    const char *c = wmconfig_get("notify_corner", "top-right");
    int right = !strstr(c, "left"), bottom = !!strstr(c, "bottom"), y = bottom ? sh - margin : margin;
    for (int i = 0; i < nn; i++) {
        Note *n = &notes[i]; int x = right ? sw - n->w - margin : margin;
        if (bottom) y -= n->h;
        XMoveResizeWindow(dpy, n->win, x, y, (unsigned)n->w, (unsigned)n->h);
        y = bottom ? y - 6 : y + n->h + 6;
    }
    XFlush(dpy);
}

static void relayout_all(void) {
    for (int i = 0; i < nn; i++) { layout(&notes[i]); }
    place_all(); for (int i = 0; i < nn; i++) draw(&notes[i]); XFlush(dpy);
}

/* ---------- D-Bus signals / lifecycle ---------- */
static void emit(const char *name, unsigned id, const char *key, unsigned reason) {
    DBusMessage *s = dbus_message_new_signal(OBJ_PATH, BUS_NAME, name); if (!s) return;
    if (key) dbus_message_append_args(s, DBUS_TYPE_UINT32, &id, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
    else dbus_message_append_args(s, DBUS_TYPE_UINT32, &id, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, s, NULL); dbus_message_unref(s); dbus_connection_flush(conn);
}

static void close_note(unsigned id, unsigned reason) {
    for (int i = 0; i < nn; i++) if (notes[i].id == id) {
        if (notes[i].icon_pm) XFreePixmap(dpy, notes[i].icon_pm);
        XDestroyWindow(dpy, notes[i].win); memmove(&notes[i], &notes[i + 1], (size_t)(nn - i - 1) * sizeof *notes); nn--;
        place_all(); emit("NotificationClosed", id, NULL, reason); return;
    }
}

typedef struct { const char *app, *icon, *summary, *body, *image_path, *entry; unsigned replaces; int timeout, urgency, value, nact; char act[MAXACT][2][96]; } Req;

static unsigned add_or_replace(const Req *q) {
    if (wmthemex_changed(&th)) { load_theme(); relayout_all(); }
    Note *n = NULL;
    for (int i = 0; i < nn; i++) if (q->replaces && notes[i].id == q->replaces) n = &notes[i];
    if (!n) {
        if (nn == MAXN) close_note(notes[0].id, 4);
        n = &notes[nn++]; memset(n, 0, sizeof *n); n->id = next_id++;
        XSetWindowAttributes a; a.override_redirect = True; a.event_mask = ButtonPressMask | ExposureMask | PointerMotionMask | LeaveWindowMask; a.background_pixel = th.face;
        n->win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, 340, 40, 0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWEventMask | CWBackPixel, &a);
        XClassHint ch = { "wmnotify", "WMNotify" }; XSetClassHint(dpy, n->win, &ch); XStoreName(dpy, n->win, "wmnotify");
        XMapRaised(dpy, n->win);
    }
    if (n->icon_pm) { XFreePixmap(dpy, n->icon_pm); n->icon_pm = 0; }
    snprintf(n->app, sizeof n->app, "%s", q->app); snprintf(n->summary, sizeof n->summary, "%s", q->summary);
    snprintf(n->body, sizeof n->body, "%s", q->body); snprintf(n->icon, sizeof n->icon, "%s", q->icon);
    snprintf(n->entry, sizeof n->entry, "%s", q->entry ? q->entry : "");
    strip_markup(n->summary); strip_markup(n->body);
    n->urgency = q->urgency; n->value = q->value; n->hover = -1; n->nact = q->nact;
    for (int i = 0; i < q->nact; i++) { snprintf(n->act[i].key, 64, "%s", q->act[i][0]); snprintf(n->act[i].label, 96, "%s", q->act[i][1]); }
    find_icon(n, q->image_path);
    int to = q->timeout, def = atoi(wmconfig_get("notify_timeout", "5000"));
    n->expire = (to == 0 || n->urgency == 2) ? 0 : now_ms() + (to < 0 ? (def > 0 ? def : 5000) : to);   /* critical stays until clicked */
    layout(n); place_all(); draw(n); XFlush(dpy);
    return n->id;
}

/* ---------- message parsing ---------- */
static int is(DBusMessageIter *it, int type) { return dbus_message_iter_get_arg_type(it) == type; }

static int parse_notify(DBusMessage *m, Req *q) {
    DBusMessageIter it, arr, dict, ent, var; dbus_int32_t to = -1; dbus_uint32_t rep = 0;
    memset(q, 0, sizeof *q); q->app = q->icon = q->summary = q->body = ""; q->value = -1; q->timeout = -1;
    if (!dbus_message_iter_init(m, &it)) return 0;
    #define STR(field) do { if (!is(&it, DBUS_TYPE_STRING)) return 0; dbus_message_iter_get_basic(&it, &q->field); dbus_message_iter_next(&it); } while (0)
    STR(app);
    if (!is(&it, DBUS_TYPE_UINT32)) return 0;
    dbus_message_iter_get_basic(&it, &rep); dbus_message_iter_next(&it); q->replaces = rep;
    STR(icon); STR(summary); STR(body);
    if (!is(&it, DBUS_TYPE_ARRAY)) return 0;                                    /* actions: key, label, key, label ... */
    dbus_message_iter_recurse(&it, &arr);
    while (is(&arr, DBUS_TYPE_STRING)) {
        const char *k, *l; dbus_message_iter_get_basic(&arr, &k); dbus_message_iter_next(&arr);
        if (!is(&arr, DBUS_TYPE_STRING)) break;
        dbus_message_iter_get_basic(&arr, &l); dbus_message_iter_next(&arr);
        if (!strcmp(k, "default")) { snprintf(q->act[MAXACT - 1][0], 96, "default"); continue; }   /* default = click on the body, no button */
        if (q->nact < MAXACT - 1) { snprintf(q->act[q->nact][0], 96, "%s", k); snprintf(q->act[q->nact][1], 96, "%s", l); q->nact++; }
    }
    dbus_message_iter_next(&it);
    if (!is(&it, DBUS_TYPE_ARRAY)) return 0;                                    /* hints a{sv} */
    dbus_message_iter_recurse(&it, &dict);
    while (is(&dict, DBUS_TYPE_DICT_ENTRY)) {
        const char *key; dbus_message_iter_recurse(&dict, &ent);
        if (is(&ent, DBUS_TYPE_STRING)) {
            dbus_message_iter_get_basic(&ent, &key); dbus_message_iter_next(&ent);
            if (is(&ent, DBUS_TYPE_VARIANT)) {
                dbus_message_iter_recurse(&ent, &var); int t = dbus_message_iter_get_arg_type(&var);
                if (!strcmp(key, "urgency") && t == DBUS_TYPE_BYTE) { unsigned char b; dbus_message_iter_get_basic(&var, &b); q->urgency = b > 2 ? 1 : b; }
                else if (!strcmp(key, "value") && t == DBUS_TYPE_INT32) { dbus_int32_t v; dbus_message_iter_get_basic(&var, &v); q->value = v < 0 ? 0 : v > 100 ? 100 : v; }
                else if (!strcmp(key, "value") && t == DBUS_TYPE_UINT32) { dbus_uint32_t v; dbus_message_iter_get_basic(&var, &v); q->value = v > 100 ? 100 : (int)v; }
                else if ((!strcmp(key, "image-path") || !strcmp(key, "image_path")) && t == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&var, &q->image_path);
                else if (!strcmp(key, "desktop-entry") && t == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&var, &q->entry);
            }
        }
        dbus_message_iter_next(&dict);
    }
    dbus_message_iter_next(&it);
    if (is(&it, DBUS_TYPE_INT32)) { dbus_message_iter_get_basic(&it, &to); q->timeout = to; }
    return 1;
}

static void reply(DBusMessage *m, DBusMessage *r) { if (r) { dbus_connection_send(conn, r, NULL); dbus_message_unref(r); } (void)m; }

static void handle(DBusMessage *m) {
    const char *member = dbus_message_get_member(m);
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL || !member) return;
    if (!strcmp(member, "GetCapabilities")) {
        DBusMessage *r = dbus_message_new_method_return(m); DBusMessageIter it, arr;
        static const char *caps[] = { "body", "actions", "icon-static" };
        dbus_message_iter_init_append(r, &it); dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
        for (int i = 0; i < 3; i++) dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &caps[i]);
        dbus_message_iter_close_container(&it, &arr); reply(m, r);
    } else if (!strcmp(member, "GetServerInformation")) {
        const char *a = "wmnotify", *b = "wm-desktop", *c = "0.2", *d = "1.2"; DBusMessage *r = dbus_message_new_method_return(m);
        dbus_message_append_args(r, DBUS_TYPE_STRING, &a, DBUS_TYPE_STRING, &b, DBUS_TYPE_STRING, &c, DBUS_TYPE_STRING, &d, DBUS_TYPE_INVALID); reply(m, r);
    } else if (!strcmp(member, "Notify")) {
        Req q;
        if (!parse_notify(m, &q)) { reply(m, dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS, "malformed Notify call")); }
        else { dbus_uint32_t id = add_or_replace(&q); DBusMessage *r = dbus_message_new_method_return(m); dbus_message_append_args(r, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID); reply(m, r); }
    } else if (!strcmp(member, "CloseNotification")) {
        dbus_uint32_t id = 0;
        if (!dbus_message_get_args(m, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID)) reply(m, dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS, "CloseNotification takes one uint32"));
        else { close_note(id, 3); reply(m, dbus_message_new_method_return(m)); }
    }
    dbus_connection_flush(conn);
}

/* ---------- X events ---------- */
static Note *note_of(Window w) { for (int i = 0; i < nn; i++) if (notes[i].win == w) return &notes[i]; return NULL; }

static int hit(const Note *n, int x, int y) {
    for (int i = 0; i < n->nact; i++) if (x >= n->act[i].x && x < n->act[i].x + n->act[i].w && y >= n->act[i].y && y < n->act[i].y + n->act[i].h) return i;
    return -1;
}

static void x_event(XEvent *ev) {
    Note *n = note_of(ev->xany.window); if (!n) return;
    if (ev->type == Expose) { if (ev->xexpose.count == 0) draw(n); }
    else if (ev->type == MotionNotify || ev->type == LeaveNotify) {
        int h = ev->type == LeaveNotify ? -1 : hit(n, ev->xmotion.x, ev->xmotion.y);
        if (h != n->hover) { n->hover = h; draw(n); }
    } else if (ev->type == ButtonPress) {
        unsigned id = n->id; int h = hit(n, ev->xbutton.x, ev->xbutton.y);
        if (h >= 0) { char key[64]; snprintf(key, sizeof key, "%s", n->act[h].key); emit("ActionInvoked", id, key, 0); }
        else if (!strcmp(n->act[MAXACT - 1].key, "default")) emit("ActionInvoked", id, "default", 0);
        close_note(id, 2);
    }
}

int main(void) {
    dpy = XOpenDisplay(NULL); if (!dpy) { fputs("wmnotify: cannot open display\n", stderr); return 1; }
    XSetErrorHandler(x_error);
    int files = load_theme();
    if (files < 0) { fputs("wmnotify: no usable fonts (is fontconfig set up?)\n", stderr); return 1; }
    fprintf(stderr, "wmnotify: theme from %d Window Maker defaults file%s\n", files, files == 1 ? "" : "s");
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
        if (nn && (timeout < 0 || timeout > 1000)) timeout = 1000;                   /* also wakes to notice a theme switch */
        if (XPending(dpy) == 0 && dbus_connection_get_dispatch_status(conn) != DBUS_DISPATCH_DATA_REMAINS) {
            struct pollfd p[2] = { { dfd, POLLIN, 0 }, { ConnectionNumber(dpy), POLLIN, 0 } };
            poll(p, 2, timeout);
        }
        dbus_connection_read_write(conn, 0);
        DBusMessage *m; while ((m = dbus_connection_pop_message(conn))) { handle(m); dbus_message_unref(m); }
        while (XPending(dpy)) { XEvent ev; XNextEvent(dpy, &ev); x_event(&ev); }
        if (nn && wmthemex_changed(&th)) { load_theme(); relayout_all(); }
        long long t = now_ms();
        for (int i = 0; i < nn;) { if (notes[i].expire && notes[i].expire <= t) close_note(notes[i].id, 1); else i++; }
    }
}
