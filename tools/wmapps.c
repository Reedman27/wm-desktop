#define _POSIX_C_SOURCE 200809L
/* wmapps - CLI front end for the WMApps registry.
 *   wmapps list                          one line per app
 *   wmapps json                          full registry as JSON (for Dock/Settings/IPC)
 *   wmapps menu [-t term] [-o file]      categorized Window Maker root-menu plist
 *   wmapps launch ID [FILE|URL...]       start an app detached, no shell (-t term for Terminal=true apps)
 *   wmapps exec ID [FILE|URL...]         print the command that launch would run (argv, one per line)
 *   wmapps icons [-s SIZE]               which icon file each app gets, and how it was found (theme/converted/category/generated)
 *   wmapps skipped                       desktop files that are broken (not merely hidden), with the reason
 * Common options: --dir D (extra data dir, highest priority), --desktop NAME (OnlyShowIn/NotShowIn). */
#include "../core/applications/wmapps.h"
#include "../core/applications/wmicons.h"
#include "../core/applications/wmmenu.h"
#include "../core/configuration/wmconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int cmp(const void *a, const void *b) {
    const WMApp *x = a, *y = b; int c = strcasecmp(x->name, y->name); return c ? c : strcmp(x->id, y->id);
}

static void jstr(FILE *o, const char *s) {
    fputc('"', o);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(o, "\\%c", *p);
        else if (*p < 0x20) fprintf(o, "\\u%04x", *p);
        else fputc(*p, o);
    }
    fputc('"', o);
}

static void usage(void) {
    fputs("usage: wmapps list | json | skipped | icons [-s SIZE] | menu [-t term] [-o file]\n"
          "              | launch ID [FILE|URL...] [-t term] | exec ID [FILE|URL...]\n"
          "       options: --dir D  --desktop NAME\n", stderr);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1], *term = NULL, *out = NULL, *dir = NULL, *desktop = NULL, *id = NULL; int isize = 64;
    const char **files = calloc((size_t)argc, sizeof *files); size_t nfiles = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) term = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--desktop") && i + 1 < argc) desktop = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) isize = atoi(argv[++i]);
        else if (!strcmp(cmd, "launch") || !strcmp(cmd, "exec")) { if (!id) id = argv[i]; else files[nfiles++] = argv[i]; }
        else { usage(); return 2; }
    }
    WMAppRegistry r; wmapps_scan(&r, desktop, dir); wmicons_set_dir(dir);
    qsort(r.apps, r.count, sizeof *r.apps, cmp);
    FILE *o = out ? fopen(out, "w") : stdout; if (!o) { perror(out); return 1; }
    int rc = 0;
    char termbuf[512];
    if (!term) { snprintf(termbuf, sizeof termbuf, "%s", wmconfig_get("terminal", "xterm -e")); term = termbuf; }

    if (!strcmp(cmd, "list")) {
        for (size_t i = 0; i < r.count; i++)
            fprintf(o, "%-12s %-8s %-32s %s\n", r.apps[i].category,
                    r.apps[i].origin == WMAPP_FLATPAK ? "flatpak" : "native", r.apps[i].name, r.apps[i].id);
    } else if (!strcmp(cmd, "json")) {
        fputs("[\n", o);
        for (size_t i = 0; i < r.count; i++) {
            WMApp *a = &r.apps[i];
            fputs("  {\"id\":", o); jstr(o, a->id); fputs(",\"path\":", o); jstr(o, a->path);
            fputs(",\"name\":", o); jstr(o, a->name); fputs(",\"generic_name\":", o); jstr(o, a->generic_name);
            fputs(",\"comment\":", o); jstr(o, a->comment); fputs(",\"exec\":", o); jstr(o, a->exec);
            fputs(",\"argv\":[", o);
            for (size_t k = 0; k < a->argc; k++) { if (k) fputc(',', o); jstr(o, a->argv[k]); }
            fputs("],\"icon\":", o); jstr(o, a->icon); fputs(",\"wm_class\":", o); jstr(o, a->wm_class);
            fputs(",\"category\":", o); jstr(o, a->category); fputs(",\"mime_types\":", o); jstr(o, a->mime_types);
            fputs(",\"work_dir\":", o); jstr(o, a->work_dir); fputs(",\"flatpak_id\":", o); jstr(o, a->flatpak_id);
            fprintf(o, ",\"terminal\":%s,\"origin\":\"%s\"}%s\n", a->terminal ? "true" : "false",
                    a->origin == WMAPP_FLATPAK ? "flatpak" : "native", i + 1 < r.count ? "," : "");
        }
        fputs("]\n", o);
    } else if (!strcmp(cmd, "icons")) {
        int missing = 0;
        for (size_t i = 0; i < r.count; i++) {
            char path[4096]; WMIconSource src = wmicons_for_app(&r.apps[i], isize, path, sizeof path);
            if (src == WMICON_NONE) missing++;
            fprintf(o, "%-10s %-32s %s\n", wmicons_source_name(src), r.apps[i].id, path[0] ? path : "-");
        }
        if (missing) { fprintf(stderr, "wmapps: %d app(s) without an icon (is the icon cache writable?)\n", missing); rc = 1; }
    } else if (!strcmp(cmd, "skipped")) {
        for (size_t i = 0; i < r.nskipped; i++) fprintf(o, "%s\n", r.skipped[i]);
    } else if (!strcmp(cmd, "menu")) {
        fputs("// Generated by wmapps - do not edit by hand.\n", o);
        wmmenu_emit_applications(o, &r, term);
        fputc('\n', o);
    } else if (!strcmp(cmd, "launch") || !strcmp(cmd, "exec")) {
        const WMApp *a = id ? wmapps_find(&r, id) : NULL;
        if (!a) { fprintf(stderr, "wmapps: no such app: %s\n", id ? id : "(missing ID)"); rc = 1; }
        else if (!strcmp(cmd, "exec")) {
            size_t n; char **v = wmapps_expand(a, files, nfiles, &n);
            if (a->terminal) { char **t; size_t tn; if (!wmapps_tokenize(term, &t, &tn)) { for (size_t k = 0; k < tn; k++) fprintf(o, "%s\n", t[k]); wmapps_argv_free(t); } }
            for (size_t k = 0; k < n; k++) fprintf(o, "%s\n", v[k]);
            wmapps_argv_free(v);
        } else {
            char err[512];
            if (wmapps_launch(a, files, nfiles, term, err, sizeof err)) { fprintf(stderr, "wmapps: %s\n", err); rc = 1; }
        }
    } else { usage(); rc = 2; }
    if (out) fclose(o);
    free(files); wmapps_free(&r);
    return rc;
}
