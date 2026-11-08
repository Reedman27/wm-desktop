/* WMApps - application registry for the wm-desktop add-ons.
 *
 * One scan of XDG + Flatpak .desktop files; Menu, Dock and Settings consume this
 * instead of scanning on their own. Implements the Desktop Entry Specification
 * subset a launcher needs (see README "WMApps compliance" for what is NOT covered).
 *
 * Commands are never handed to a shell by WMApps itself: Exec is tokenized into an
 * argv template, field codes are expanded per token, and wmapps_launch() uses execvp().
 * Where a shell string is unavoidable (Window Maker menu/Dock entries), it is built
 * by quoting every argv element, so Exec contents cannot inject shell syntax. */
#ifndef WMAPPS_H
#define WMAPPS_H
#include <stddef.h>

typedef enum { WMAPP_NATIVE, WMAPP_FLATPAK } WMAppOrigin;

typedef struct {
    char  *id;           /* desktop-file ID per spec: "org.gnome.Calculator", "vendor-tool" (no .desktop) */
    char  *path;         /* source file */
    char  *name;         /* localized Name */
    char  *generic_name; /* localized GenericName, "" if none */
    char  *comment;      /* localized Comment, "" if none */
    char  *exec;         /* safe shell-quoted command line, no files (for WM menu/Dock text) */
    char **argv;         /* Exec tokenized, field codes still present (NULL terminated) */
    size_t argc;
    char  *icon;         /* name or absolute path, "" if none */
    char  *wm_class;     /* StartupWMClass, "" if none */
    char  *category;     /* WM menu category, see wmapps_category_names */
    char  *mime_types;   /* MimeType= verbatim ("a/b;c/d;"), "" if none */
    char  *work_dir;     /* Path=, "" if none */
    char  *flatpak_id;   /* X-Flatpak=, "" if none */
    int    terminal;
    WMAppOrigin origin;
} WMApp;

typedef struct {
    WMApp *apps; size_t count, cap;
    char **skipped; size_t nskipped;   /* "path: reason" for entries that are broken (not merely hidden) */
} WMAppRegistry;

/* ---- scanning ---- */

/* Scan all standard locations. XDG precedence: an ID found in an earlier directory
 * hides the same ID in later ones, even when the earlier file is Hidden/NoDisplay/
 * filtered out (that is how users mask system entries).
 * 'desktop': colon-separated desktop names for OnlyShowIn/NotShowIn;
 *            NULL = $XDG_CURRENT_DESKTOP, or "WindowMaker" if unset.
 * 'extra_dir': highest-priority data dir (has an applications/ subdir); may be NULL. */
void wmapps_scan(WMAppRegistry *r, const char *desktop, const char *extra_dir);
/* Ordered list of data dirs that are scanned (each has an applications/ subdir). */
size_t wmapps_dirs(char ***out, const char *extra_dir);
void wmapps_dirs_free(char **v, size_t n);
void wmapps_free(WMAppRegistry *r);

/* ---- lookup / identity ---- */

/* By desktop-file ID (exact, then case-insensitive), then by StartupWMClass. */
const WMApp *wmapps_find(const WMAppRegistry *r, const char *id_or_class);
/* Running window (WM_CLASS instance/class) -> app. Used by the Dock. */
const WMApp *wmapps_match_window(const WMAppRegistry *r, const char *instance, const char *wm_class);
/* basename of the program Exec starts ("" if none). */
const char *wmapps_bin(const WMApp *a, char *buf, size_t n);

/* ---- commands (no shell) ---- */

/* Split an Exec-style string into arguments (double quotes with backslash escapes,
 * plus tolerated single quotes, as real-world files use them). 0 ok, -1 malformed. */
int wmapps_tokenize(const char *s, char ***argv, size_t *argc);
void wmapps_argv_free(char **argv);
/* Single-quote 's' for /bin/sh unless it is already plain. Result is malloc'd. */
char *wmapps_shell_quote(const char *s);
/* 0: Exec takes no files, 1: one file/URL (%f %u), 2: a list (%F %U). */
int wmapps_accepts_files(const WMApp *a);
/* Expand the Exec template. 'files' are paths or URIs and are converted as each
 * field code requires. Returns a malloc'd NULL-terminated argv (free with
 * wmapps_argv_free) and its length in *n, or NULL on failure. */
char **wmapps_expand(const WMApp *a, const char *const *files, size_t nfiles, size_t *n);
/* Shell-safe command line (every argument quoted). If 'terminal' is non-NULL and the
 * app has Terminal=true, the tokenized terminal command is put in front. malloc'd. */
char *wmapps_shell_command(const WMApp *a, const char *const *files, size_t nfiles, const char *terminal);
/* Start the app detached (double fork + execvp, no shell). With several files and an
 * app that only takes one (%f/%u) it starts one instance per file. Returns 0 on
 * success; on failure -1 with a message in 'err' (exec failures are reported). */
int wmapps_launch(const WMApp *a, const char *const *files, size_t nfiles,
                  const char *terminal, char *err, size_t errlen);

extern const char *const wmapps_category_names[];  /* NULL terminated, menu order */

#endif
