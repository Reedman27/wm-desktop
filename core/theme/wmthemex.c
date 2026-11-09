#define _POSIX_C_SOURCE 200809L
#include "wmthemex.h"
#include <stdio.h>
#include <string.h>

static unsigned long alloc_rgb(Display *dpy, unsigned rgb) {
    XColor c; memset(&c, 0, sizeof c);
    c.red = (unsigned short)(((rgb >> 16) & 255) * 257); c.green = (unsigned short)(((rgb >> 8) & 255) * 257);
    c.blue = (unsigned short)((rgb & 255) * 257); c.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), &c)) return BlackPixel(dpy, DefaultScreen(dpy));
    return c.pixel;
}

/* colour string -> 0xRRGGBB; own parser first, then the X server's colour database (named colours) */
static unsigned resolve(Display *dpy, const char *s, unsigned fallback) {
    unsigned rgb;
    if (wmtheme_color(s, &rgb)) return rgb;
    XColor c;
    if (s && XParseColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), s, &c))
        return (unsigned)(c.red >> 8) << 16 | (unsigned)(c.green >> 8) << 8 | (unsigned)(c.blue >> 8);
    return fallback;
}

static XFontStruct *open_font(Display *dpy, const char *spec) {
    char pat[12][160]; int n = wmtheme_font_patterns(spec, pat, 12);
    for (int i = 0; i < n; i++) { XFontStruct *f = XLoadQueryFont(dpy, pat[i]); if (f) return f; }
    return NULL;
}

int wmthemex_load(Display *dpy, WMThemeX *x, const char *user, const char *global) {
    WMTheme t; int files = wmtheme_load(&t, user, global);
    unsigned face = resolve(dpy, t.menu_text_back, 0xAAAAAA), tface = resolve(dpy, t.menu_title_back, 0x000000), l, d;
    int had = x->font != NULL;
    wmtheme_shades(face, &l, &d); x->face_rgb = face;
    x->face = alloc_rgb(dpy, face); x->light = alloc_rgb(dpy, l); x->dark = alloc_rgb(dpy, d);
    x->text = alloc_rgb(dpy, resolve(dpy, t.menu_text_color, 0x000000));
    x->disabled = alloc_rgb(dpy, resolve(dpy, t.menu_disabled_color, 0x7F7F7F));
    x->border = alloc_rgb(dpy, resolve(dpy, t.frame_border_color, 0x000000));
    wmtheme_shades(tface, &l, &d);
    x->title_face = alloc_rgb(dpy, tface); x->title_light = alloc_rgb(dpy, l); x->title_dark = alloc_rgb(dpy, d);
    x->title_text = alloc_rgb(dpy, resolve(dpy, t.menu_title_color, 0xFFFFFF));
    x->hi = alloc_rgb(dpy, resolve(dpy, t.highlight_color, 0xFFFFFF));
    x->hi_text = alloc_rgb(dpy, resolve(dpy, t.highlight_text_color, 0x000000));

    XFontStruct *f = open_font(dpy, t.menu_text_font), *tf = open_font(dpy, t.menu_title_font);
    if (!f) f = XLoadQueryFont(dpy, "fixed");
    if (!tf) tf = XLoadQueryFont(dpy, "fixed");
    if (!f || !tf) return -1;
    if (had) { if (x->font) XFreeFont(dpy, x->font); if (x->title_font) XFreeFont(dpy, x->title_font); }
    x->font = f; x->title_font = tf; x->mtime = wmtheme_mtime();
    return files;
}

int wmthemex_changed(const WMThemeX *x) { return wmtheme_mtime() != x->mtime; }
