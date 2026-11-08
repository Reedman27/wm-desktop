/* WMIcons - find an icon Window Maker can load for every application.
 *
 * Window Maker only loads PNG/XPM, so the lookup is a ladder; the first rung that works wins:
 *   1. THEME      the app's own Icon= (name or absolute path), then its Flatpak ID, desktop-file ID,
 *                 StartupWMClass and binary name, searched in every installed icon theme
 *   2. CONVERTED  an SVG-only icon rendered to a cached PNG (rsvg-convert, ImageMagick or Inkscape)
 *   3. CATEGORY   the theme's generic icon for the app's menu category (applications-games, ...)
 *   4. GENERATED  a drawn tile: category colour, bevelled border, the app's initial
 * Rungs 2 and 4 write into $XDG_CACHE_HOME/wm-desktop/icons (default ~/.cache/wm-desktop/icons). */
#ifndef WMICONS_H
#define WMICONS_H
#include "wmapps.h"
#include <stddef.h>

typedef enum { WMICON_NONE, WMICON_THEME, WMICON_CONVERTED, WMICON_CATEGORY, WMICON_GENERATED } WMIconSource;

/* Writes a PNG/XPM path to 'out' and says which rung found it. Returns WMICON_NONE (and an empty
 * string) only when nothing exists and the cache directory cannot be written. 'size' = wanted pixels. */
WMIconSource wmicons_for_app(const WMApp *a, int size, char *out, size_t outlen);
const char *wmicons_source_name(WMIconSource s);
/* Extra highest-priority data dir (has an icons/ subdir), the same one given to wmapps_scan(). NULL = none. */
void wmicons_set_dir(const char *dir);

/* Low level: one icon name -> best file in the installed themes, no fallbacks and no conversion.
 * Returns 1 for png/xpm (loadable by Window Maker), 2 for svg-only (needs conversion), 0 if nothing. */
int wmicons_resolve(const char *icon, int size, char *out, size_t outlen);
#endif
