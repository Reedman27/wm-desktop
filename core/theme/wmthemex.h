/* WMThemeX - a WMTheme's colours, textures and fonts on an X display.
 *
 * Fonts are opened through Xft, exactly as Window Maker does (its font strings are fontconfig patterns),
 * so they are antialiased and UTF-8 capable. Textures (solid and all gradient kinds) and PNG/XPM icons use
 * libwraster, Window Maker's own image library, when built with -DHAVE_WRASTER; without it gradients are
 * drawn by a plain two-colour fallback and icons are unavailable. */
#ifndef WMTHEMEX_H
#define WMTHEMEX_H
#include "wmtheme.h"
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>

typedef struct {
    unsigned long face, light, dark, text, disabled, border;   /* menu body: MenuTextBack/Color + bevel */
    unsigned long title_face, title_light, title_dark, title_text;   /* menu title bar */
    unsigned long hi, hi_text;                                 /* selection: HighlightColor / HighlightTextColor */
    unsigned face_rgb;
    XftFont *font, *title_font;
    XftColor xtext, xtitle_text, xhi_text, xdisabled;          /* the same colours for Xft text drawing */
    WMTexture text_tex, title_tex;                             /* MenuTextBack / MenuTitleBack in full */
    void *rctx;                                                /* libwraster context, created on first use */
    int have_xft_colors;
    long mtime;                                                /* mtime of the user defaults file when loaded */
} WMThemeX;

/* (Re)load from Window Maker's config and allocate on 'dpy'. Fonts and Xft colours from a previous call are freed;
 * pixel cells are not (a few per theme change, none on TrueColor). Zero the struct before the first call.
 * 'user'/'global' override file locations (NULL = defaults). Always succeeds with something usable;
 * returns the number of config files read (0 = built-in Window Maker defaults). Returns -1 if no font loads at all. */
int wmthemex_load(Display *dpy, WMThemeX *x, const char *user, const char *global);
/* 1 if the user's defaults file changed since wmthemex_load. */
int wmthemex_changed(const WMThemeX *x);

/* A new w*h pixmap holding the texture (caller XFreePixmap), or 0. */
Pixmap wmthemex_texture(Display *dpy, WMThemeX *x, const WMTexture *t, int w, int h);
/* Loads an image scaled to fit size*size (aspect kept), flattened onto 'bg'; ow/oh get the result size.
 * 0 without libwraster or when the file cannot be read. */
Pixmap wmthemex_icon(Display *dpy, WMThemeX *x, const char *path, int size, unsigned bg_rgb, int *ow, int *oh);

/* UTF-8 text helpers over an XftFont */
int wmx_text_width(Display *dpy, XftFont *f, const char *utf8, int len);
int wmx_font_height(const XftFont *f);
unsigned wmthemex_rgb(Display *dpy, const char *color, unsigned fallback);   /* a theme colour string as 0xRRGGBB */
#endif
