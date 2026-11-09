/* WMTheme - read the colours and fonts of the *running Window Maker's* look from its own config,
 * so wm-desktop's windows match whatever theme (style) is active. No Xlib needed here.
 *
 * Where the values live: a Window Maker theme is applied by merging its style keys into the
 * `WindowMaker` defaults file, so the "current theme" is simply that file:
 *     $WMAKER_USER_ROOT/Defaults/WindowMaker   (default ~/GNUstep/Defaults/WindowMaker)
 * layered over the global one (<sysconfdir>/WindowMaker/WindowMaker, found in /etc and
 * /usr/local/etc, or $WMTHEME_GLOBAL). Keys the user file lacks come from the global file, then
 * from Window Maker's own built-in defaults, exactly as Window Maker resolves them.
 *
 * Textures such as (dgradient, "rgb:a6/a6/b6", "rgb:51/55/61") are kept in full in menu_text_tex /
 * menu_title_tex (kind plus colours); menu_text_back / menu_title_back still hold the first colour for callers
 * that only want a flat fill. Pixmap textures keep their fallback colour. */
#ifndef WMTHEME_H
#define WMTHEME_H
#include <stddef.h>

typedef enum { WMTEX_SOLID, WMTEX_HGRAD, WMTEX_VGRAD, WMTEX_DGRAD, WMTEX_MHGRAD, WMTEX_MVGRAD, WMTEX_MDGRAD } WMTexKind;
#define WMTEX_MAXC 12
typedef struct { WMTexKind kind; int n; char color[WMTEX_MAXC][64]; } WMTexture;   /* n >= 1 */

typedef struct {
    /* colour strings exactly as written in the style file (X colour names, #rrggbb, rgb:rr/gg/bb) */
    char menu_text_color[64], menu_title_color[64], menu_disabled_color[64];
    char highlight_color[64], highlight_text_color[64], frame_border_color[64];
    char menu_text_back[64], menu_title_back[64];          /* texture reduced to its first colour */
    char menu_text_font[256], menu_title_font[256];        /* fontconfig-style, e.g. "Sans:bold:pixelsize=12" */
    WMTexture menu_text_tex, menu_title_tex;               /* the same textures, with every colour and the gradient kind */
    int files_read;                                        /* how many defaults files contributed */
} WMTheme;

void wmtheme_defaults(WMTheme *t);                         /* Window Maker's built-in values */
/* Fill 't' from the defaults files. 'user' / 'global' override the default locations (NULL = default). */
int wmtheme_load(WMTheme *t, const char *user, const char *global);
/* Path of one of Window Maker's user defaults files, e.g. "WMWindowAttributes" (honours $WMAKER_USER_ROOT like wmaker). 0 if no HOME. */
int wmtheme_defaults_path(const char *domain, char *out, size_t n);
/* Path of the user's WindowMaker defaults file (the one to watch for theme changes). 0 if no HOME. */
int wmtheme_user_path(char *out, size_t n);
/* Modification time (seconds) of the user file, 0 if missing; poll it to notice theme changes. */
long wmtheme_mtime(void);

/* "#rgb", "#rrggbb", "rgb:r/g/b", "black", "white", "grayNN"/"greyNN" and a few basics -> 0xRRGGBB.
 * Returns 1 on success; callers with an X display fall back to XParseColor for other names. */
int wmtheme_color(const char *s, unsigned *rgb);
/* Light and dark edge colours derived from a face colour, for bevels. */
void wmtheme_shades(unsigned face, unsigned *light, unsigned *dark);
/* Candidate core-X-font patterns for a fontconfig-style spec ("Sans:bold:pixelsize=12", comma
 * separated lists allowed, XLFDs passed through). Best first; the caller should end with "fixed". */
int wmtheme_font_patterns(const char *spec, char out[][160], int max);
#endif
