/* WMMenu - turns a WMAppRegistry into a Window Maker plist menu.
 *
 * Commands come from wmapps_shell_command() (every argv element quoted), then get the
 * escaping Window Maker needs: it runs menu/Dock commands through ExpandOptions(), where
 * '\' is an escape and %w %W %a %A %s %d are live options, so '\' -> "\\" and '%' -> "\%".
 * Finally plist string escaping. */
#ifndef WMMENU_H
#define WMMENU_H
#include "wmapps.h"
#include <stdio.h>

/* Escape a command for Window Maker's menu/Dock command expansion. malloc'd. */
char *wmmenu_wm_escape(const char *cmd);
/* Write 's' as a quoted plist string. */
void wmmenu_plist_string(FILE *o, const char *s);

/* Sorts the registry by name, then writes the categorized ("Applications", ...) submenu. */
void wmmenu_emit_applications(FILE *o, WMAppRegistry *r, const char *terminal);

/* Writes a complete root menu. 'tmpl' is the user's template text (NULL = built-in);
 * the line consisting of @APPLICATIONS@ is replaced by the generated submenu. */
void wmmenu_emit_root(FILE *o, WMAppRegistry *r, const char *terminal, const char *tmpl);
#endif
