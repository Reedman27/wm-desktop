/* WMConfig - the one settings file shared by every wm-desktop program and both backends.
 * ~/.config/wm-desktop/settings.conf, lines of "key = value", '#' comments. */
#ifndef WMCONFIG_H
#define WMCONFIG_H
#include <stddef.h>
/* Path of the settings file ($WMCONFIG_FILE overrides, for tests). */
void wmconfig_path(char *out, size_t n);
/* Value for key, or 'def' when unset. Result is valid until the next call. */
const char *wmconfig_get(const char *key, const char *def);
/* Set key (adds or replaces the line, keeps comments). Returns 0 on success. */
int wmconfig_set(const char *key, const char *value);
#endif
