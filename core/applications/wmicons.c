#define _POSIX_C_SOURCE 200809L
#include "wmicons.h"
#include "../configuration/wmconfig.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SVG_PENALTY 24      /* a raster within +-24 px beats rendering an SVG */
#define MAX_THEMES 512      /* (base dir, theme) pairs, including ones that do not exist */

/* ------------------------------------------------------------------ helpers */

static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }

static int has_suffix(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n > m && !strcasecmp(s + n - m, suf);
}

static uint32_t fnv(const char *s, uint32_t h) {
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static int in_path(const char *prog, char *full, size_t n) {
    const char *p = getenv("PATH"); if (!p) return 0;
    char *d = strdup(p), *save = d; int ok = 0;
    if (!d) return 0;
    for (char *s = d, *e; !ok; s = e + 1) {
        e = strchr(s, ':'); if (e) *e = 0;
        snprintf(full, n, "%s/%s", *s ? s : ".", prog);
        if (access(full, X_OK) == 0 && is_file(full)) ok = 1;
        if (!e) break;
    }
    free(save); return ok;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
    return s;
}

/* ------------------------------------------------------------ theme parsing */

typedef struct { char path[256]; int size, min, max, thr, type; } IDir;   /* type 0 fixed, 1 scalable, 2 threshold */
typedef struct { char key[1024]; int exists, has_index, n, cap; IDir *dirs; char inherits[1024]; } Theme;
static Theme theme_cache[MAX_THEMES];
static int ntheme_cache;

static int word_in(const char *list, const char *w) {      /* comma separated */
    size_t n = strlen(w);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ','); size_t l = e ? (size_t)(e - p) : strlen(p);
        while (l && (*p == ' ')) { p++; l--; }
        while (l && p[l-1] == ' ') l--;
        if (l == n && !strncmp(p, w, n)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

static Theme *theme_load(const char *tdir) {
    for (int i = 0; i < ntheme_cache; i++) if (!strcmp(theme_cache[i].key, tdir)) return &theme_cache[i];
    if (ntheme_cache == MAX_THEMES) return NULL;
    Theme *t = &theme_cache[ntheme_cache++];
    memset(t, 0, sizeof *t); snprintf(t->key, sizeof t->key, "%s", tdir);
    t->exists = is_dir(tdir);
    if (!t->exists) return t;
    char ip[4200]; snprintf(ip, sizeof ip, "%s/index.theme", tdir);
    FILE *f = fopen(ip, "r"); if (!f) return t;
    t->has_index = 1;
    char dirlist[16384] = ""; char *line = NULL; size_t cap = 0;
    char group[256] = ""; IDir cur; int cur_ok = 0, cur_apps = 0, cur_scale = 1; char ctx[64] = "";
    cur.size = cur.min = cur.max = cur.thr = cur.type = 0; cur.path[0] = 0;
    #define FLUSH() do { if (cur_ok && cur_apps && cur_scale == 1 && !strstr(cur.path, "symbolic")) { \
        if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 64; t->dirs = realloc(t->dirs, (size_t)t->cap * sizeof *t->dirs); if (!t->dirs) abort(); } \
        t->dirs[t->n++] = cur; } } while (0)
    while (getline(&line, &cap, f) >= 0) {
        char *s = trim(line);
        if (*s == '[') {
            FLUSH();
            char *e = strchr(s, ']'); if (e) *e = 0;
            snprintf(group, sizeof group, "%s", s + 1);
            memset(&cur, 0, sizeof cur); cur_ok = 0; cur_apps = 0; cur_scale = 1; ctx[0] = 0;
            if (strcmp(group, "Icon Theme") && dirlist[0] && word_in(dirlist, group)) {
                snprintf(cur.path, sizeof cur.path, "%s", group); cur.min = -1; cur.max = -1;
                cur_apps = strstr(group, "apps") != NULL;   /* refined by Context= below */
            }
            continue;
        }
        char *eq = strchr(s, '='); if (!eq || *s == '#') continue;
        *eq = 0; char *k = trim(s), *v = trim(eq + 1);
        if (!strcmp(group, "Icon Theme")) {
            if (!strcmp(k, "Inherits")) snprintf(t->inherits, sizeof t->inherits, "%s", v);
            else if (!strcmp(k, "Directories")) snprintf(dirlist, sizeof dirlist, "%s", v);
        } else if (cur.path[0]) {
            if (!strcmp(k, "Size")) { cur.size = atoi(v); cur_ok = 1; if (cur.min < 0) cur.min = cur.max = cur.size; }
            else if (!strcmp(k, "MinSize")) cur.min = atoi(v);
            else if (!strcmp(k, "MaxSize")) cur.max = atoi(v);
            else if (!strcmp(k, "Threshold")) cur.thr = atoi(v);
            else if (!strcmp(k, "Scale")) cur_scale = atoi(v);
            else if (!strcmp(k, "Type")) cur.type = !strcasecmp(v, "Scalable") ? 1 : !strcasecmp(v, "Fixed") ? 0 : 2;
            else if (!strcmp(k, "Context")) { snprintf(ctx, sizeof ctx, "%s", v); cur_apps = !strcasecmp(v, "Applications"); }
        }
    }
    FLUSH();
    #undef FLUSH
    for (int i = 0; i < t->n; i++) {             /* defaults per spec */
        IDir *d = &t->dirs[i];
        if (d->min < 0) d->min = d->size;
        if (d->max < 0) d->max = d->size;
        if (d->type == 2 && !d->thr) d->thr = 2;
    }
    free(line); fclose(f);
    return t;
}

/* Upscaling blurs, downscaling does not: a directory whose largest size is below 'want' is
 * penalized so that a bigger icon wins over a smaller one even if the smaller is numerically closer. */
static int too_small(int dist, int largest, int want) { return largest < want ? dist * 4 + 1000 : dist; }

static int dir_distance(const IDir *d, int want) {
    if (d->type == 0) return too_small(abs(d->size - want), d->size, want);
    if (d->type == 1) return too_small(want < d->min ? d->min - want : want > d->max ? want - d->max : 0, d->max, want);
    if (want < d->size - d->thr) return (d->size - d->thr) - want;
    if (want > d->size + d->thr) return too_small(want - (d->size + d->thr), d->size + d->thr, want);
    return 0;
}

typedef struct { char path[4096]; long score; int kind; int found; } Best;   /* kind 1 raster, 2 svg */

static void consider(Best *b, const char *dir, const char *name, int dist, int have_conv) {
    static const char *const ext[] = { "png", "xpm", "svg", NULL };
    for (int i = 0; ext[i]; i++) {
        char p[4096]; snprintf(p, sizeof p, "%s/%s.%s", dir, name, ext[i]);
        if (!is_file(p)) continue;
        int kind = i == 2 ? 2 : 1;
        long sc = kind == 1 ? dist : (have_conv ? SVG_PENALTY : 100000 + dist);
        if (!b->found || sc < b->score) { snprintf(b->path, sizeof b->path, "%s", p); b->score = sc; b->kind = kind; b->found = 1; }
    }
}

static int have_converter(void);

static void theme_lookup(Best *b, const char *tdir, const char *name, int want) {
    Theme *t = theme_load(tdir); if (!t || !t->exists) return;
    int conv = have_converter();
    char dir[8600];
    if (t->has_index) {
        for (int i = 0; i < t->n; i++) {
            snprintf(dir, sizeof dir, "%s/%s", tdir, t->dirs[i].path);
            consider(b, dir, name, dir_distance(&t->dirs[i], want), conv);
        }
        return;
    }
    DIR *d = opendir(tdir); if (!d) return;       /* no index.theme: infer from NxN / scalable directory names */
    struct dirent *e;
    while ((e = readdir(d))) {
        int w = 0, h = 0;
        if (sscanf(e->d_name, "%dx%d", &w, &h) == 2 && w > 0 && !strchr(e->d_name, '@')) {
            snprintf(dir, sizeof dir, "%s/%s/apps", tdir, e->d_name); consider(b, dir, name, too_small(abs(w - want), w, want), conv);
        } else if (!strcmp(e->d_name, "scalable")) {
            snprintf(dir, sizeof dir, "%s/scalable/apps", tdir); consider(b, dir, name, 0, conv);
        }
    }
    closedir(d);
}

/* ---------------------------------------------------------- theme ordering */

typedef struct { char **v; int n; } Names;
static int names_has(const Names *s, const char *x) { for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i], x)) return 1; return 0; }
static void names_add(Names *s, const char *x) {
    s->v = realloc(s->v, ((size_t)s->n + 1) * sizeof *s->v); if (!s->v) abort();
    s->v[s->n] = strdup(x); if (!s->v[s->n]) abort(); s->n++;
}
static void names_free(Names *s) { for (int i = 0; i < s->n; i++) free(s->v[i]); free(s->v); memset(s, 0, sizeof *s); }

typedef struct { char **v; size_t n; } Bases;      /* icon base dirs in spec order */

static void chain_add(Names *c, const Bases *bs, const char *theme, int depth) {
    if (!*theme || names_has(c, theme) || depth > 8) return;
    names_add(c, theme);
    for (size_t i = 0; i < bs->n; i++) {
        char td[8192]; snprintf(td, sizeof td, "%s/%s", bs->v[i], theme);
        Theme *t = theme_load(td);
        if (t && t->has_index && t->inherits[0]) {
            char inh[1024]; snprintf(inh, sizeof inh, "%s", t->inherits);
            for (char *s = inh, *e; ; s = e + 1) {
                e = strchr(s, ','); if (e) *e = 0;
                chain_add(c, bs, trim(s), depth + 1);
                if (!e) break;
            }
            break;
        }
    }
}

static int gtk_theme(char *out, size_t n) {
    const char *xc = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    const char *files[2]; char f1[4096], f2[4096];
    if (xc && *xc) snprintf(f1, sizeof f1, "%s/gtk-3.0/settings.ini", xc); else snprintf(f1, sizeof f1, "%s/.config/gtk-3.0/settings.ini", h ? h : "");
    snprintf(f2, sizeof f2, "%s/.gtkrc-2.0", h ? h : "");
    files[0] = f1; files[1] = f2;
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(files[i], "r"); if (!f) continue;
        char *line = NULL; size_t cap = 0; int ok = 0;
        while (!ok && getline(&line, &cap, f) >= 0) {
            char *s = trim(line);
            if (strncmp(s, "gtk-icon-theme-name", 19)) continue;
            char *eq = strchr(s, '='); if (!eq) continue;
            char *v = trim(eq + 1);
            if (*v == '"') { v++; char *q = strchr(v, '"'); if (q) *q = 0; }
            if (*v) { snprintf(out, n, "%s", v); ok = 1; }
        }
        free(line); fclose(f);
        if (ok) return 1;
    }
    return 0;
}

static char extra_dir[4096];
void wmicons_set_extra_dir(const char *dir) { snprintf(extra_dir, sizeof extra_dir, "%s", dir ? dir : ""); }

static void bases_build(Bases *bs) {
    const char *home = getenv("HOME"); char b[4096];
    char **dirs; size_t nd = wmapps_dirs(&dirs, extra_dir[0] ? extra_dir : NULL);
    bs->v = calloc(nd + 2, sizeof *bs->v); bs->n = 0; if (!bs->v) abort();
    snprintf(b, sizeof b, "%s/.icons", home ? home : ""); bs->v[bs->n++] = strdup(b);
    for (size_t i = 0; i < nd; i++) { snprintf(b, sizeof b, "%s/icons", dirs[i]); bs->v[bs->n++] = strdup(b); }
    wmapps_dirs_free(dirs, nd);
}
static void bases_free(Bases *bs) { for (size_t i = 0; i < bs->n; i++) free(bs->v[i]); free(bs->v); }

static void strip_ext(char *name) {
    size_t n = strlen(name);
    if (n > 4 && (has_suffix(name, ".png") || has_suffix(name, ".xpm") || has_suffix(name, ".svg"))) name[n - 4] = 0;
}

int wmicons_resolve(const char *icon, int size, char *out, size_t outlen) {
    if (!icon || !*icon) return 0;
    if (icon[0] == '/') {
        if (!is_file(icon)) return 0;
        snprintf(out, outlen, "%s", icon); return has_suffix(icon, ".svg") ? 2 : 1;
    }
    char name[512]; snprintf(name, sizeof name, "%s", icon); strip_ext(name);   /* some entries write Icon=foo.png */

    Bases bs; bases_build(&bs);
    Names chain = {0}; char tn[256];
    const char *env = getenv("WMICONS_THEME");
    if (env && *env) chain_add(&chain, &bs, env, 0);
    const char *cfg = wmconfig_get("icon_theme", "");
    if (cfg && *cfg) chain_add(&chain, &bs, cfg, 0);
    if (gtk_theme(tn, sizeof tn)) chain_add(&chain, &bs, tn, 0);
    chain_add(&chain, &bs, "hicolor", 0);                    /* the spec's mandatory fallback */
    for (size_t i = 0; i < bs.n; i++) {                      /* then whatever else is installed */
        DIR *d = opendir(bs.v[i]); struct dirent *e;
        while (d && (e = readdir(d))) if (e->d_name[0] != '.') chain_add(&chain, &bs, e->d_name, 0);
        if (d) closedir(d);
    }

    Best b; memset(&b, 0, sizeof b);
    for (int t = 0; t < chain.n && !b.found; t++)
        for (size_t i = 0; i < bs.n && !b.found; i++) {
            char td[8192]; snprintf(td, sizeof td, "%s/%s", bs.v[i], chain.v[t]);
            theme_lookup(&b, td, name, size);
        }
    names_free(&chain); bases_free(&bs);
    if (!b.found) {                                          /* legacy pixmaps */
        static const char *const pix[] = { "/usr/share/pixmaps", "/usr/local/share/pixmaps", NULL };
        for (int i = 0; pix[i]; i++) consider(&b, pix[i], name, 0, have_converter());
    }
    if (!b.found) return 0;
    snprintf(out, outlen, "%s", b.path); return b.kind;
}

/* ------------------------------------------------------------ SVG -> PNG */

static const char *const converters[] = { "rsvg-convert", "magick", "convert", "inkscape", NULL };
static int have_converter(void) { char full[4096]; for (int i = 0; converters[i]; i++) if (in_path(converters[i], full, sizeof full)) return 1; return 0; }

static int cache_dir(char *out, size_t n) {
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/wm-desktop/icons", x);
    else if (h && *h) snprintf(out, n, "%s/.cache/wm-desktop/icons", h);
    else return 0;
    char p[4096]; snprintf(p, sizeof p, "%s", out);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    return mkdir(p, 0755) == 0 || errno == EEXIST;
}

static void safe_name(const char *in, char *out, size_t n) {
    size_t k = 0;
    for (const char *p = strrchr(in, '/') ? strrchr(in, '/') + 1 : in; *p && k + 1 < n && k < 60; p++)
        out[k++] = (isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_') ? *p : '_';
    out[k] = 0;
}

static int run_quiet(char *const *argv, int timeout_s) {
    pid_t p = fork(); if (p < 0) return -1;
    if (p == 0) {
        int dn = open("/dev/null", O_RDWR); if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        execvp(argv[0], argv); _exit(127);
    }
    for (int i = 0; i < timeout_s * 50; i++) {
        int st; pid_t r = waitpid(p, &st, WNOHANG);
        if (r == p) return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
        if (r < 0) return -1;
        struct timespec ts = { 0, 20000000 }; nanosleep(&ts, NULL);
    }
    kill(p, SIGKILL); waitpid(p, NULL, 0); return -1;
}

int wmicons_convert_svg(const char *svg, int size, char *out, size_t outlen) {
    struct stat st; char dir[4096];
    if (stat(svg, &st) || !cache_dir(dir, sizeof dir)) return 0;
    char sz[16], nm[80], key[4400]; snprintf(sz, sizeof sz, "%d", size);
    safe_name(svg, nm, sizeof nm); { char *dot = strrchr(nm, '.'); if (dot && !strcasecmp(dot, ".svg")) *dot = 0; }
    snprintf(key, sizeof key, "%s|%lld|%ld|%ld|%lld", svg,
             (long long)st.st_dev, (long)st.st_mtim.tv_sec,
             st.st_mtim.tv_nsec, (long long)st.st_size);
    char final[8300]; snprintf(final, sizeof final, "%s/%s-%d-%08x.png", dir, nm, size, (unsigned)fnv(key, 2166136261u));
    if (is_file(final)) { snprintf(out, outlen, "%s", final); return 1; }           /* cached, SVG unchanged */

    char tmp[8400], px[32], full[4096]; snprintf(tmp, sizeof tmp, "%s.%d.tmp.png", final, (int)getpid());
    char s2[32]; snprintf(s2, sizeof s2, "%dx%d", size, size); snprintf(px, sizeof px, "%d", size);
    for (int i = 0; converters[i]; i++) {
        if (!in_path(converters[i], full, sizeof full)) continue;
        char *argv_rsvg[]  = { "rsvg-convert", "-a", "-w", px, "-h", px, "-o", tmp, (char *)svg, NULL };
        char *argv_magick[] = { (char *)converters[i], "-background", "none", "-density", "384", (char *)svg, "-resize", s2, tmp, NULL };
        char *argv_ink[]   = { "inkscape", "--export-type=png", "-w", px, "-h", px, "-o", tmp, (char *)svg, NULL };
        char **argv = i == 0 ? argv_rsvg : i == 3 ? argv_ink : argv_magick;
        if (run_quiet(argv, 20) == 0 && is_file(tmp) && !stat(tmp, &st) && st.st_size > 0 && !rename(tmp, final)) {
            snprintf(out, outlen, "%s", final); return 1;
        }
        unlink(tmp);
    }
    return 0;
}

/* ------------------------------------------------------- generated tile */

static const char GLYPH_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#";
static const char *const GLYPH[][7] = {   /* 5x7 bitmap font, '#' = pixel */
 {".###.","#...#","#...#","#####","#...#","#...#","#...#"}, {"####.","#...#","#...#","####.","#...#","#...#","####."},
 {".###.","#...#","#....","#....","#....","#...#",".###."}, {"####.","#...#","#...#","#...#","#...#","#...#","####."},
 {"#####","#....","#....","####.","#....","#....","#####"}, {"#####","#....","#....","####.","#....","#....","#...."},
 {".###.","#...#","#....","#.###","#...#","#...#",".####"}, {"#...#","#...#","#...#","#####","#...#","#...#","#...#"},
 {".###.","..#..","..#..","..#..","..#..","..#..",".###."}, {"..###","...#.","...#.","...#.","...#.","#..#.",".##.."},
 {"#...#","#..#.","#.#..","##...","#.#..","#..#.","#...#"}, {"#....","#....","#....","#....","#....","#....","#####"},
 {"#...#","##.##","#.#.#","#.#.#","#...#","#...#","#...#"}, {"#...#","##..#","#.#.#","#..##","#...#","#...#","#...#"},
 {".###.","#...#","#...#","#...#","#...#","#...#",".###."}, {"####.","#...#","#...#","####.","#....","#....","#...."},
 {".###.","#...#","#...#","#...#","#.#.#","#..#.",".##.#"}, {"####.","#...#","#...#","####.","#.#..","#..#.","#...#"},
 {".####","#....","#....",".###.","....#","....#","####."}, {"#####","..#..","..#..","..#..","..#..","..#..","..#.."},
 {"#...#","#...#","#...#","#...#","#...#","#...#",".###."}, {"#...#","#...#","#...#","#...#","#...#",".#.#.","..#.."},
 {"#...#","#...#","#...#","#.#.#","#.#.#","##.##","#...#"}, {"#...#","#...#",".#.#.","..#..",".#.#.","#...#","#...#"},
 {"#...#","#...#",".#.#.","..#..","..#..","..#..","..#.."}, {"#####","....#","...#.","..#..",".#...","#....","#####"},
 {".###.","#...#","#..##","#.#.#","##..#","#...#",".###."}, {"..#..",".##..","..#..","..#..","..#..","..#..",".###."},
 {".###.","#...#","....#","...#.","..#..",".#...","#####"}, {"####.","....#","....#",".###.","....#","....#","####."},
 {"...#.","..##.",".#.#.","#..#.","#####","...#.","...#."}, {"#####","#....","####.","....#","....#","#...#",".###."},
 {"..##.",".#...","#....","####.","#...#","#...#",".###."}, {"#####","....#","...#.","..#..",".#...",".#...",".#..."},
 {".###.","#...#","#...#",".###.","#...#","#...#",".###."}, {".###.","#...#","#...#",".####","....#","...#.",".##.."},
 {".#.#.",".#.#.","#####",".#.#.","#####",".#.#.",".#.#."},
};

static const char *const TILE_COLORS[][3] = {      /* background, border, highlight - muted so no tile shouts */
 {"#4f6d8f","#33495f","#7d9bbd"}, {"#5d8a6b","#3d5c47","#8ab998"}, {"#8f6a4f","#5f4533","#bd977d"}, {"#7a5d8f","#503d5f","#a68abd"},
 {"#8f4f5d","#5f333d","#bd7d8a"}, {"#4f8f87","#335f59","#7dbdb5"}, {"#8f854f","#5f5933","#bdb37d"}, {"#5d6a8f","#3d465f","#8a97bd"},
 {"#6d8f4f","#485f33","#9bbd7d"}, {"#8f5f4f","#5f3f33","#bd8d7d"}, {"#4f7a8f","#33505f","#7da8bd"}, {"#7f6f6f","#554a4a","#aa9b9b"},
};

int wmicons_generate(const char *key, const char *label, int size, char *out, size_t outlen) {
    char dir[4096]; if (!cache_dir(dir, sizeof dir)) return 0;
    int S = size < 32 ? 32 : size > 128 ? 128 : size;
    int ci = (int)(fnv(key ? key : "", 2166136261u) % (sizeof TILE_COLORS / sizeof *TILE_COLORS));
    int gi = (int)strlen(GLYPH_CHARS) - 1;                               /* '#': non-alphanumeric first character */
    for (const char *p = label ? label : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) && c < 128) { const char *g = strchr(GLYPH_CHARS, toupper(c)); if (g) gi = (int)(g - GLYPH_CHARS); break; }
        if (c >= 128) break;
    }
    char nm[80]; safe_name(key && *key ? key : "app", nm, sizeof nm);
    char final[8300]; snprintf(final, sizeof final, "%s/gen-%s-%d-%08x.xpm", dir, nm, S, (unsigned)fnv(label ? label : "", fnv(key ? key : "", 2166136261u)));
    if (is_file(final)) { snprintf(out, outlen, "%s", final); return 1; }

    char *px = malloc((size_t)S * (size_t)S); if (!px) return 0;
    int R = S / 7;                                                       /* corner radius */
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            char c = '.';                                                /* '.' fill */
            int cx = x < R ? R - x : x >= S - R ? x - (S - R - 1) : 0;
            int cy = y < R ? R - y : y >= S - R ? y - (S - R - 1) : 0;
            if (cx && cy && cx * cx + cy * cy > R * R) c = 't';          /* transparent corner */
            else {
                int edge = x == 0 || y == 0 || x == S - 1 || y == S - 1 || (cx && cy && cx * cx + cy * cy > (R - 1) * (R - 1));
                if (edge) c = 'o';
                else if ((y == 1 && x > R / 2 && x < S - R / 2) || (x == 1 && y > R / 2 && y < S - R / 2)) c = 'h';   /* top/left light edge */
            }
            px[y * S + x] = c;
        }
    int k = S / 14; if (k < 2) k = 2;
    int gx = (S - 5 * k) / 2, gy = (S - 7 * k) / 2;
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 5; c++)
            if (GLYPH[gi][r][c] == '#')
                for (int dy = 0; dy < k; dy++) for (int dx = 0; dx < k; dx++) px[(gy + r * k + dy) * S + gx + c * k + dx] = 'x';

    char tmp[8400]; snprintf(tmp, sizeof tmp, "%s.%d.tmp", final, (int)getpid());
    FILE *f = fopen(tmp, "w"); if (!f) { free(px); return 0; }
    fprintf(f, "/* XPM */\nstatic char *icon[] = {\n\"%d %d 5 1\",\n\"t c None\",\n\". c %s\",\n\"o c %s\",\n\"h c %s\",\n\"x c #f2f2f2\",\n",
            S, S, TILE_COLORS[ci][0], TILE_COLORS[ci][1], TILE_COLORS[ci][2]);
    for (int y = 0; y < S; y++) { fputc('"', f); fwrite(px + (size_t)y * S, 1, (size_t)S, f); fputs(y + 1 < S ? "\",\n" : "\"\n", f); }
    fputs("};\n", f); free(px);
    if (fclose(f) || rename(tmp, final)) { unlink(tmp); return 0; }
    snprintf(out, outlen, "%s", final); return 1;
}

/* ------------------------------------------------------------ per-app */

const char *wmicons_source_name(WMIconSource s) {
    return s == WMICON_FILE ? "file" : s == WMICON_CONVERTED ? "converted" : s == WMICON_GENERATED ? "generated" : "none";
}

WMIconSource wmicons_for_app(const WMApp *a, int size, char *out, size_t outlen, int *guessed) {
    char cand[6][512]; int nc = 0; char bin[256];
    #define ADDC(s) do { const char *s_ = (s); int dup_ = 0; if (s_ && *s_) { for (int i_ = 0; i_ < nc; i_++) if (!strcmp(cand[i_], s_)) dup_ = 1; \
                         if (!dup_ && nc < 6) snprintf(cand[nc++], sizeof cand[0], "%s", s_); } } while (0)
    ADDC(a->icon);
    char low[512]; snprintf(low, sizeof low, "%s", a->wm_class); for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
    ADDC(a->flatpak_id); ADDC(a->id); ADDC(low);
    const char *tail = strrchr(a->id, '.'); if (tail) { char t[512]; snprintf(t, sizeof t, "%s", tail + 1); for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p); ADDC(t); }
    if (!a->flatpak_id[0]) ADDC(wmapps_bin(a, bin, sizeof bin));
    #undef ADDC
    if (guessed) *guessed = 0;
    for (int i = 0; i < nc; i++) {
        char path[4096]; int r = wmicons_resolve(cand[i], size, path, sizeof path);
        int from_icon = a->icon[0] && !strcmp(cand[i], a->icon);
        if (r == 1) { snprintf(out, outlen, "%s", path); if (guessed) *guessed = !from_icon; return WMICON_FILE; }
        if (r == 2 && wmicons_convert_svg(path, size, out, outlen)) { if (guessed) *guessed = !from_icon; return WMICON_CONVERTED; }
    }
    if (wmicons_generate(a->id, a->name, size, out, outlen)) return WMICON_GENERATED;
    return WMICON_NONE;
}
