/* WMIcons - find an icon for every application, in a form Window Maker can load.
 *
 * Window Maker loads PNG and XPM (not SVG), so the pipeline for an app is:
 *   1. theme lookup per the Icon Theme Specification: your theme (WMICONS_THEME env, the
 *      'icon_theme' setting, or GTK's gtk-icon-theme-name), its Inherits chain, hicolor,
 *      then any other installed theme, then pixmaps/. index.theme directory sizes are honoured;
 *      symbolic (monochrome) directories are skipped.
 *   2. SVG-only hits are rendered to a PNG in $XDG_CACHE_HOME/wm-desktop/icons with
 *      rsvg-convert, ImageMagick (magick/convert) or inkscape, whichever is in PATH (none required).
 *   3. If Icon= names nothing usable, other names the app is known by are tried
 *      (StartupWMClass, desktop-file ID and its last component, program name).
 *   4. Last resort: a generated XPM tile (colour from the app ID, the app's initial on it).
 * So wmicons_for_app() returns a loadable file whenever the cache directory is writable. */
#ifndef WMICONS_H
#define WMICONS_H
#include "wmapps.h"
#include <stddef.h>

typedef enum {
    WMICON_NONE = 0,     /* nothing found and nothing could be generated */
    WMICON_FILE,         /* an existing PNG/XPM from a theme or path */
    WMICON_CONVERTED,    /* SVG rendered to a cached PNG */
    WMICON_GENERATED     /* generated letter tile (app has no usable icon) */
} WMIconSource;

/* Search this extra data dir (it has an icons/ subdir) first, matching wmapps_scan()'s 'extra_dir'. NULL clears. */
void wmicons_set_extra_dir(const char *dir);

/* Resolve one icon name or absolute path. Returns 1 for png/xpm (loadable by Window Maker),
 * 2 for svg-only (convert it with wmicons_convert_svg), 0 if nothing found. 'size' = wanted pixels. */
int wmicons_resolve(const char *icon, int size, char *out, size_t outlen);

/* Render an SVG to a cached PNG of about size x size. Returns 1 and the path in 'out', or 0 when
 * no converter is installed or it failed. Re-renders only if the SVG changed. */
int wmicons_convert_svg(const char *svg, int size, char *out, size_t outlen);

/* Generated tile, cached as XPM. 'key' picks the colour, 'label' supplies the initial. 1 on success. */
int wmicons_generate(const char *key, const char *label, int size, char *out, size_t outlen);

/* Best icon file for an app (see the pipeline above). *guessed (may be NULL) is set to 1 when the
 * icon came from an alternative name rather than the Icon= value. */
WMIconSource wmicons_for_app(const WMApp *a, int size, char *out, size_t outlen, int *guessed);

const char *wmicons_source_name(WMIconSource s);   /* "file", "converted", "generated", "none" */
#endif
