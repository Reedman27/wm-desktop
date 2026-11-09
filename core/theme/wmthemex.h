/* WMThemeX - allocate a WMTheme's colours and fonts on an X display. */
#ifndef WMTHEMEX_H
#define WMTHEMEX_H
#include "wmtheme.h"
#include <X11/Xlib.h>

typedef struct {
    unsigned long face, light, dark, text, disabled, border;   /* menu body: MenuTextBack/Color + bevel */
    unsigned long title_face, title_light, title_dark, title_text;   /* menu title bar */
    unsigned long hi, hi_text;                                 /* selection: HighlightColor / HighlightTextColor */
    unsigned face_rgb;
    XFontStruct *font, *title_font;
    long mtime;                                                /* mtime of the user defaults file when loaded */
} WMThemeX;

/* (Re)load from Window Maker's config and allocate on 'dpy'. Fonts from a previous call are freed; colour cells are not (a few per theme change, none on TrueColor).
 * 'user'/'global' override file locations (NULL = defaults). Always succeeds with something usable;
 * returns the number of config files read (0 = built-in Window Maker defaults). Returns -1 if no font loads at all. */
int wmthemex_load(Display *dpy, WMThemeX *x, const char *user, const char *global);
/* 1 if the user's defaults file changed since wmthemex_load. */
int wmthemex_changed(const WMThemeX *x);
#endif
