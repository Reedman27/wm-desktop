#define _POSIX_C_SOURCE 200809L
#include "wmicons.h"
#include "wmapps.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char path[4096]; int size; int kind; /* 1 raster, 2 svg */ int found; } Best;

static int exists(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }

/* lower is better; sizes at/above the wanted one beat smaller ones (downscale > upscale) */
static long score(int have, int want) { return have >= want ? have - want : (long)(want - have) * 4 + 1000; }

static void consider(Best *b, const char *dir, const char *name, int size, int want) {
    static const char *const ext[] = { "png", "xpm", "svg", NULL };
    for (int i = 0; ext[i]; i++) {
        char p[4096]; snprintf(p, sizeof p, "%s/%s.%s", dir, name, ext[i]);
        if (!exists(p)) continue;
        int kind = i == 2 ? 2 : 1;
        int sz = kind == 2 ? want : size;         /* svg scales: treat as exact */
        long sc = score(sz, want) + (kind == 2 ? 100000 : 0);   /* any raster beats svg */
        long cur = b->found ? score(b->size, want) + (b->kind == 2 ? 100000 : 0) : -1;
        if (!b->found || sc < cur) { snprintf(b->path, sizeof b->path, "%s", p); b->size = sz; b->kind = kind; b->found = 1; }
    }
}

static void search_theme(Best *b, const char *themedir, const char *name, int want) {
    DIR *d = opendir(themedir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int w = 0, h = 0; char sub[8192];
        if (sscanf(e->d_name, "%dx%d", &w, &h) == 2 && w > 0) {
            snprintf(sub, sizeof sub, "%s/%s/apps", themedir, e->d_name); consider(b, sub, name, w, want);
        } else if (!strcmp(e->d_name, "scalable")) {
            snprintf(sub, sizeof sub, "%s/scalable/apps", themedir); consider(b, sub, name, want, want);
        } else if (!strcmp(e->d_name, "apps")) {     /* old layout: theme/apps/<size> */
            continue;
        }
    }
    closedir(d);
}

int wmicons_resolve(const char *icon, int size, char *out, size_t outlen) {
    if (!icon || !*icon) return 0;
    if (icon[0] == '/') {
        if (!exists(icon)) return 0;
        snprintf(out, outlen, "%s", icon);
        size_t n = strlen(icon); return (n > 4 && !strcmp(icon + n - 4, ".svg")) ? 2 : 1;
    }
    /* strip a stray extension: some entries write Icon=foo.png */
    char name[512]; snprintf(name, sizeof name, "%s", icon);
    size_t n = strlen(name);
    if (n > 4 && (!strcmp(name + n - 4, ".png") || !strcmp(name + n - 4, ".xpm") || !strcmp(name + n - 4, ".svg"))) name[n - 4] = 0;

    Best b; memset(&b, 0, sizeof b);
    const char *home = getenv("HOME"); char base[4096];
    char **dirs; size_t nd = wmapps_dirs(&dirs, NULL);     /* same data dirs as the app scan */
    static const char *const themes_pref[] = { "hicolor", NULL };
    for (size_t i = 0; i < nd + 1; i++) {
        if (i == nd) snprintf(base, sizeof base, "%s/.icons", home ? home : "");
        else snprintf(base, sizeof base, "%s/icons", dirs[i]);
        for (int t = 0; themes_pref[t]; t++) {
            char td[8192]; snprintf(td, sizeof td, "%s/%s", base, themes_pref[t]); search_theme(&b, td, name, size);
        }
        if (b.found && b.kind == 1) break;                  /* earlier data dir wins on a raster hit */
        DIR *d = opendir(base); struct dirent *e;           /* then any other installed theme */
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "hicolor")) continue;
            char td[8192]; snprintf(td, sizeof td, "%s/%s", base, e->d_name); search_theme(&b, td, name, size);
        }
        if (d) closedir(d);
        if (b.found && b.kind == 1) break;
    }
    wmapps_dirs_free(dirs, nd);
    if (!b.found) {                                          /* legacy pixmaps */
        const char *pix[] = { "/usr/share/pixmaps", "/usr/local/share/pixmaps", NULL };
        for (int i = 0; pix[i]; i++) consider(&b, pix[i], name, size, size);
    }
    if (!b.found) return 0;
    snprintf(out, outlen, "%s", b.path); return b.kind;
}
