/* WMIcons - resolve an icon name from a .desktop file to a file Window Maker can load. */
#ifndef WMICONS_H
#define WMICONS_H
#include <stddef.h>
/* Writes the best match to 'out'. Returns 1 for png/xpm (loadable by Window Maker),
 * 2 for svg-only (needs conversion), 0 if nothing found. 'size' is the wanted pixel size. */
int wmicons_resolve(const char *icon, int size, char *out, size_t outlen);
#endif
