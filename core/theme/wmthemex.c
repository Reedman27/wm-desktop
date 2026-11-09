#define _POSIX_C_SOURCE 200809L
#include "wmthemex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_WRASTER
#include <wraster.h>
#endif

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
unsigned wmthemex_rgb(Display *dpy, const char *color, unsigned fallback) { return resolve(dpy, color, fallback); }

static void xft_color(Display *dpy, XftColor *c, unsigned rgb) {
    XRenderColor r = { (unsigned short)(((rgb >> 16) & 255) * 257), (unsigned short)(((rgb >> 8) & 255) * 257), (unsigned short)((rgb & 255) * 257), 0xFFFF };
    int s = DefaultScreen(dpy);
    XftColorAllocValue(dpy, DefaultVisual(dpy, s), DefaultColormap(dpy, s), &r, c);
}

static XftFont *open_font(Display *dpy, const char *spec) {
    int s = DefaultScreen(dpy); XftFont *f = NULL;
    if (spec && *spec) f = spec[0] == '-' ? XftFontOpenXlfd(dpy, s, spec) : XftFontOpenName(dpy, s, spec);
    if (!f) f = XftFontOpenName(dpy, s, "Sans:pixelsize=12");
    if (!f) f = XftFontOpenName(dpy, s, "fixed");
    if (!f) f = XftFontOpenName(dpy, s, "monospace:pixelsize=12");
    return f;
}

int wmthemex_load(Display *dpy, WMThemeX *x, const char *user, const char *global) {
    WMTheme t; int files = wmtheme_load(&t, user, global);
    unsigned face = resolve(dpy, t.menu_text_back, 0xAAAAAA), tface = resolve(dpy, t.menu_title_back, 0x000000), l, d;
    XftFont *f = open_font(dpy, t.menu_text_font), *tf = open_font(dpy, t.menu_title_font);
    if (!f || !tf) { if (f) XftFontClose(dpy, f); if (tf) XftFontClose(dpy, tf); return -1; }
    if (x->font) XftFontClose(dpy, x->font);
    if (x->title_font) XftFontClose(dpy, x->title_font);
    if (x->have_xft_colors) {
        int s = DefaultScreen(dpy); Visual *v = DefaultVisual(dpy, s); Colormap cm = DefaultColormap(dpy, s);
        XftColorFree(dpy, v, cm, &x->xtext); XftColorFree(dpy, v, cm, &x->xtitle_text);
        XftColorFree(dpy, v, cm, &x->xhi_text); XftColorFree(dpy, v, cm, &x->xdisabled);
    }
    wmtheme_shades(face, &l, &d); x->face_rgb = face;
    x->face = alloc_rgb(dpy, face); x->light = alloc_rgb(dpy, l); x->dark = alloc_rgb(dpy, d);
    unsigned text = resolve(dpy, t.menu_text_color, 0x000000), dis = resolve(dpy, t.menu_disabled_color, 0x7F7F7F);
    unsigned ttext = resolve(dpy, t.menu_title_color, 0xFFFFFF), hit = resolve(dpy, t.highlight_text_color, 0x000000);
    x->text = alloc_rgb(dpy, text); x->disabled = alloc_rgb(dpy, dis);
    x->border = alloc_rgb(dpy, resolve(dpy, t.frame_border_color, 0x000000));
    wmtheme_shades(tface, &l, &d);
    x->title_face = alloc_rgb(dpy, tface); x->title_light = alloc_rgb(dpy, l); x->title_dark = alloc_rgb(dpy, d);
    x->title_text = alloc_rgb(dpy, ttext);
    x->hi = alloc_rgb(dpy, resolve(dpy, t.highlight_color, 0xFFFFFF)); x->hi_text = alloc_rgb(dpy, hit);
    xft_color(dpy, &x->xtext, text); xft_color(dpy, &x->xtitle_text, ttext); xft_color(dpy, &x->xhi_text, hit); xft_color(dpy, &x->xdisabled, dis);
    x->have_xft_colors = 1;
    x->font = f; x->title_font = tf; x->text_tex = t.menu_text_tex; x->title_tex = t.menu_title_tex; x->mtime = wmtheme_mtime();
#ifdef HAVE_WRASTER
    if (!x->rctx) x->rctx = RCreateContext(dpy, DefaultScreen(dpy), NULL);
#endif
    return files;
}

int wmthemex_changed(const WMThemeX *x) { return wmtheme_mtime() != x->mtime; }

int wmx_text_width(Display *dpy, XftFont *f, const char *s, int len) {
    XGlyphInfo e; if (!f || len <= 0) return 0;
    XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, len, &e); return e.xOff;
}
int wmx_font_height(const XftFont *f) { return f ? f->ascent + f->descent : 12; }

Pixmap wmthemex_texture(Display *dpy, WMThemeX *x, const WMTexture *t, int w, int h) {
    if (w < 1 || h < 1 || t->n < 1) return 0;
    unsigned rgb[WMTEX_MAXC]; int n = t->n;
    for (int i = 0; i < n; i++) rgb[i] = resolve(dpy, t->color[i], 0x000000);
#ifdef HAVE_WRASTER
    if (x->rctx) {
        RImage *img = NULL; RColor c[WMTEX_MAXC];
        for (int i = 0; i < n; i++) { c[i].red = (rgb[i] >> 16) & 255; c[i].green = (rgb[i] >> 8) & 255; c[i].blue = rgb[i] & 255; c[i].alpha = 255; }
        if (t->kind == WMTEX_SOLID || n < 2) { img = RCreateImage((unsigned)w, (unsigned)h, 0); if (img) RFillImage(img, &c[0]); }
        else if (n == 2 && t->kind <= WMTEX_DGRAD)
            img = RRenderGradient((unsigned)w, (unsigned)h, &c[0], &c[1], t->kind == WMTEX_HGRAD ? RGRD_HORIZONTAL : t->kind == WMTEX_VGRAD ? RGRD_VERTICAL : RGRD_DIAGONAL);
        else {
            RColor *list[WMTEX_MAXC + 1]; for (int i = 0; i < n; i++) list[i] = &c[i]; list[n] = NULL;
            img = RRenderMultiGradient((unsigned)w, (unsigned)h, list, (t->kind == WMTEX_HGRAD || t->kind == WMTEX_MHGRAD) ? RGRD_HORIZONTAL : (t->kind == WMTEX_VGRAD || t->kind == WMTEX_MVGRAD) ? RGRD_VERTICAL : RGRD_DIAGONAL);
        }
        if (img) { Pixmap pm = 0; int ok = RConvertImage(x->rctx, img, &pm); RReleaseImage(img); if (ok) return pm; }
    }
#else
    (void)x;
#endif
    /* fallback: solid, or a linear ramp along one axis (diagonal drawn as vertical) */
    int s = DefaultScreen(dpy);
    Pixmap pm = XCreatePixmap(dpy, RootWindow(dpy, s), (unsigned)w, (unsigned)h, (unsigned)DefaultDepth(dpy, s)); GC gc = XCreateGC(dpy, pm, 0, NULL);
    int horiz = t->kind == WMTEX_HGRAD || t->kind == WMTEX_MHGRAD, len = horiz ? w : h;
    for (int i = 0; i < len; i++) {
        double pos = len > 1 ? (double)i / (len - 1) : 0, seg = pos * (n - 1); int k = (int)seg; if (k >= n - 1) k = n - 2; if (k < 0) k = 0;
        double fr = n < 2 ? 0 : seg - k; unsigned a = rgb[k], b = rgb[n < 2 ? 0 : k + 1]; unsigned out = 0;
        for (int sh = 16; sh >= 0; sh -= 8) { double av = (a >> sh) & 255, bv = (b >> sh) & 255; out |= (unsigned)(av + (bv - av) * fr + .5) << sh; }
        XSetForeground(dpy, gc, alloc_rgb(dpy, out));
        if (horiz) XFillRectangle(dpy, pm, gc, i, 0, 1, (unsigned)h); else XFillRectangle(dpy, pm, gc, 0, i, (unsigned)w, 1);
    }
    XFreeGC(dpy, gc); return pm;
}

Pixmap wmthemex_icon(Display *dpy, WMThemeX *x, const char *path, int size, unsigned bg, int *ow, int *oh) {
#ifdef HAVE_WRASTER
    (void)dpy;
    if (!x->rctx || !path || !*path) return 0;
    RImage *img = RLoadImage(x->rctx, path, 0); if (!img) return 0;
    int w = (int)img->width, h = (int)img->height;
    if (w > size || h > size || (w < size / 2 && h < size / 2)) {
        double sc = (double)size / (w > h ? w : h); w = (int)(w * sc + .5); h = (int)(h * sc + .5); if (w < 1) w = 1; if (h < 1) h = 1;
        RImage *s2 = RSmoothScaleImage(img, (unsigned)w, (unsigned)h); RReleaseImage(img); img = s2; if (!img) return 0;
    }
    RColor c = { (unsigned char)(bg >> 16), (unsigned char)(bg >> 8), (unsigned char)bg, 255 }; RCombineImageWithColor(img, &c);
    Pixmap pm = 0; int ok = RConvertImage(x->rctx, img, &pm); RReleaseImage(img);
    if (!ok) return 0;
    *ow = w; *oh = h; return pm;
#else
    (void)dpy; (void)x; (void)path; (void)size; (void)bg; (void)ow; (void)oh; return 0;
#endif
}
