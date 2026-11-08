#define _POSIX_C_SOURCE 200809L
#include "wmicons.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------ theme search */

static char extra_dir[4096];
void wmicons_set_dir(const char *dir) { snprintf(extra_dir, sizeof extra_dir, "%s", dir ? dir : ""); }

typedef struct {
    char raster[4096]; int rsize; int have_raster;
    char svg[4096];    int have_svg;
} Found;

static int exists(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }
static int has_ext(const char *s, const char *ext) { size_t n = strlen(s), e = strlen(ext); return n > e && !strcasecmp(s + n - e, ext); }
static int all_digits(const char *s) { if (!*s || strlen(s) > 4) return 0; for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0; return 1; }

/* lower is better; sizes at/above the wanted one beat smaller ones (downscale > upscale) */
static long score(int have, int want) { return have >= want ? have - want : (long)(want - have) * 4 + 1000; }
/* a raster that is at least 3/4 of the wanted size is good enough not to look further */
static int good(const Found *f, int want) { return f->have_raster && f->rsize * 4 >= want * 3; }

static void consider(Found *f, const char *dir, const char *name, int size, int want) {
    static const char *const ext[] = { "png", "xpm", "svg", NULL };
    for (int i = 0; ext[i]; i++) {
        char p[4096]; snprintf(p, sizeof p, "%s/%s.%s", dir, name, ext[i]);
        if (!exists(p)) continue;
        if (i == 2) {
            if (!f->have_svg) { snprintf(f->svg, sizeof f->svg, "%s", p); f->have_svg = 1; }
        } else if (!f->have_raster || score(size, want) < score(f->rsize, want)) {
            snprintf(f->raster, sizeof f->raster, "%s", p); f->rsize = size; f->have_raster = 1;
        }
    }
}

static void search_theme(Found *f, const char *themedir, const char *name, int want) {
    DIR *d = opendir(themedir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strchr(e->d_name, '@')) continue;      /* "48x48@2" holds 96px icons: skip */
        int w = 0, h = 0; char sub[8192];
        if (sscanf(e->d_name, "%dx%d", &w, &h) == 2 && w > 0) {
            snprintf(sub, sizeof sub, "%s/%s/apps", themedir, e->d_name); consider(f, sub, name, w, want);
        } else if (all_digits(e->d_name)) {                              /* theme/48/apps */
            snprintf(sub, sizeof sub, "%s/%s/apps", themedir, e->d_name); consider(f, sub, name, atoi(e->d_name), want);
        } else if (!strcmp(e->d_name, "scalable")) {
            snprintf(sub, sizeof sub, "%s/scalable/apps", themedir); consider(f, sub, name, want, want);
        } else if (!strcmp(e->d_name, "apps")) {                         /* old layout: theme/apps/<size> */
            snprintf(sub, sizeof sub, "%s/apps", themedir);
            DIR *a = opendir(sub); struct dirent *x;
            while (a && (x = readdir(a))) {
                char s2[8704]; snprintf(s2, sizeof s2, "%s/%s", sub, x->d_name);
                if (all_digits(x->d_name)) consider(f, s2, name, atoi(x->d_name), want);
                else if (!strcmp(x->d_name, "scalable")) consider(f, s2, name, want, want);
            }
            if (a) closedir(a);
        }
    }
    closedir(d);
}

/* One icon name or absolute path -> best raster and first svg in the installed themes. */
static int find_icon(const char *icon, int want, Found *f) {
    memset(f, 0, sizeof *f);
    if (!icon || !*icon) return 0;
    if (icon[0] == '/') {
        if (!exists(icon)) return 0;
        if (has_ext(icon, ".svg")) { snprintf(f->svg, sizeof f->svg, "%s", icon); f->have_svg = 1; }
        else { snprintf(f->raster, sizeof f->raster, "%s", icon); f->rsize = want; f->have_raster = 1; }
        return 1;
    }
    char name[512]; snprintf(name, sizeof name, "%s", icon);          /* some entries write Icon=foo.png */
    if (has_ext(name, ".png") || has_ext(name, ".xpm") || has_ext(name, ".svg")) name[strlen(name) - 4] = 0;
    const char *home = getenv("HOME"); char base[4096], td[8192];
    char **dirs; size_t nd = wmapps_dirs(&dirs, extra_dir[0] ? extra_dir : NULL);   /* same data dirs as the app scan */
    for (size_t i = 0; i < nd + 1 && !good(f, want); i++) {
        if (i == nd) snprintf(base, sizeof base, "%s/.icons", home ? home : "");
        else snprintf(base, sizeof base, "%s/icons", dirs[i]);
        snprintf(td, sizeof td, "%s/hicolor", base); search_theme(f, td, name, want);
        DIR *d = opendir(base); struct dirent *e;                     /* then any other installed theme */
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "hicolor")) continue;
            snprintf(td, sizeof td, "%s/%s", base, e->d_name); search_theme(f, td, name, want);
        }
        if (d) closedir(d);
    }
    wmapps_dirs_free(dirs, nd);
    if (!good(f, want)) {                                             /* legacy pixmaps */
        consider(f, "/usr/share/pixmaps", name, want, want);
        consider(f, "/usr/local/share/pixmaps", name, want, want);
    }
    return f->have_raster || f->have_svg;
}

int wmicons_resolve(const char *icon, int size, char *out, size_t outlen) {
    Found f; if (!find_icon(icon, size, &f)) return 0;
    if (f.have_raster) { snprintf(out, outlen, "%s", f.raster); return 1; }
    snprintf(out, outlen, "%s", f.svg); return 2;
}

/* ------------------------------------------------------------------- cache */

static int mkdirs(const char *path) {
    char tmp[4096]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
    struct stat st; return !stat(tmp, &st) && S_ISDIR(st.st_mode) && !access(tmp, W_OK);
}

static int cache_dir(char *out, size_t n) {
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME"); struct stat st;
    if (x && *x) snprintf(out, n, "%s/wm-desktop/icons", x);
    else if (h && *h && !stat(h, &st) && S_ISDIR(st.st_mode)) snprintf(out, n, "%s/.cache/wm-desktop/icons", h);
    else return 0;                                                    /* never invent a missing $HOME */
    return mkdirs(out);
}

static unsigned long hash(const char *s) { unsigned long h = 5381; for (; *s; s++) h = h * 33 + (unsigned char)*s; return h & 0xffffffffUL; }
static void safe_name(const char *s, char *out, size_t n) {
    size_t j = 0;
    for (; *s && j + 1 < n; s++) out[j++] = (isalnum((unsigned char)*s) || *s == '.' || *s == '-' || *s == '_') ? *s : '_';
    out[j] = 0;
}
static int is_png(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    unsigned char m[8]; size_t k = fread(m, 1, 8, f); fclose(f);
    return k == 8 && !memcmp(m, "\x89PNG\r\n\x1a\n", 8);
}

/* ------------------------------------------------------ svg -> png (no shell) */

static int run(const char *const argv[]) {
    pid_t pid = fork(); if (pid < 0) return 0;
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execvp(argv[0], (char *const *)argv); _exit(127);
    }
    int st; while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return 0;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int in_path(const char *prog) {
    const char *path = getenv("PATH"); if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = strdup(path), *save = NULL; int ok = 0; if (!copy) return 0;
    for (char *d = strtok_r(copy, ":", &save); d && !ok; d = strtok_r(NULL, ":", &save)) {
        char p[4096]; snprintf(p, sizeof p, "%s/%s", *d ? d : ".", prog); ok = !access(p, X_OK);
    }
    free(copy); return ok;
}

/* Renders 'svg' to a cached PNG of about size x size. Returns 1 and the path in 'out' on success. */
static int convert_svg(const char *svg, int size, char *out, size_t outlen) {
    char cd[4096]; if (!cache_dir(cd, sizeof cd)) return 0;
    char base[200], dest[4608], tmp[4700]; const char *b = strrchr(svg, '/'); b = b ? b + 1 : svg;
    safe_name(b, base, sizeof base);
    snprintf(dest, sizeof dest, "%s/%s-%08lx-%d.png", cd, base, hash(svg), size);
    struct stat a, c;
    if (!stat(dest, &c) && !stat(svg, &a) && c.st_mtime >= a.st_mtime && is_png(dest)) { snprintf(out, outlen, "%s", dest); return 1; }

    char sz[16], geo[32], expf[4800];
    snprintf(sz, sizeof sz, "%d", size); snprintf(geo, sizeof geo, "%dx%d", size, size);
    snprintf(tmp, sizeof tmp, "%s.%d.tmp.png", dest, (int)getpid());
    snprintf(expf, sizeof expf, "--export-filename=%s", tmp);
    const char *rsvg[]  = { "rsvg-convert", "-a", "-w", sz, "-h", sz, "-o", tmp, svg, NULL };
    const char *magick[] = { "magick", "-background", "none", "-density", "384", svg, "-resize", geo, tmp, NULL };
    const char *convert[] = { "convert", "-background", "none", "-density", "384", svg, "-resize", geo, tmp, NULL };
    const char *inkscape[] = { "inkscape", "--export-type=png", expf, "-w", sz, "-h", sz, svg, NULL };
    const char *const *cmds[] = { rsvg, magick, convert, inkscape, NULL };
    for (int i = 0; cmds[i]; i++) {
        if (!in_path(cmds[i][0])) continue;
        if (run(cmds[i]) && is_png(tmp) && !rename(tmp, dest)) { snprintf(out, outlen, "%s", dest); return 1; }
        unlink(tmp);
    }
    return 0;
}

/* Best usable file from one lookup: a good-sized raster, else a converted svg, else a small raster. */
static WMIconSource pick(const Found *f, int size, WMIconSource raster_src, WMIconSource conv_src, char *out, size_t outlen) {
    if (f->have_raster && (!f->have_svg || f->rsize * 4 >= size * 3)) { snprintf(out, outlen, "%s", f->raster); return raster_src; }
    if (f->have_svg && convert_svg(f->svg, size, out, outlen)) return conv_src;
    if (f->have_raster) { snprintf(out, outlen, "%s", f->raster); return raster_src; }
    return WMICON_NONE;
}

/* ------------------------------------------------------- category fallbacks */

typedef struct { const char *cat; const char *names[4]; unsigned rgb; } CatInfo;
static const CatInfo cats[] = {
    { "Accessories", { "applications-utilities", "accessories-text-editor", NULL }, 0x7A8CA5 },
    { "Development", { "applications-development", "applications-engineering", NULL }, 0x4F7F6A },
    { "Education",   { "applications-education", "applications-science", NULL }, 0xB8863B },
    { "Games",       { "applications-games", NULL }, 0xA8483E },
    { "Graphics",    { "applications-graphics", NULL }, 0x9055A2 },
    { "Internet",    { "applications-internet", "internet-web-browser", NULL }, 0x3F78B5 },
    { "Multimedia",  { "applications-multimedia", "applications-audio", "applications-video", NULL }, 0xC0642B },
    { "Office",      { "applications-office", NULL }, 0x4C8C9A },
    { "Settings",    { "preferences-system", "preferences-other", NULL }, 0x6E6E6E },
    { "System",      { "applications-system", "utilities-system-monitor", "utilities-terminal", NULL }, 0x58606E },
    { "Other",       { "application-x-executable", "applications-other", NULL }, 0x8A7F6A },
};
static const CatInfo *cat_lookup(const char *c) {
    size_t n = sizeof cats / sizeof *cats;
    for (size_t i = 0; i < n; i++) if (c && !strcmp(c, cats[i].cat)) return &cats[i];
    return &cats[n - 1];
}

/* ---------------------------------------------------------- generated tile */

static const char glyph_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789?";
static const char *const font[][7] = {
    {"01110","10001","10001","11111","10001","10001","10001"}, {"11110","10001","10001","11110","10001","10001","11110"},
    {"01110","10001","10000","10000","10000","10001","01110"}, {"11110","10001","10001","10001","10001","10001","11110"},
    {"11111","10000","10000","11110","10000","10000","11111"}, {"11111","10000","10000","11110","10000","10000","10000"},
    {"01110","10001","10000","10111","10001","10001","01111"}, {"10001","10001","10001","11111","10001","10001","10001"},
    {"01110","00100","00100","00100","00100","00100","01110"}, {"00111","00010","00010","00010","00010","10010","01100"},
    {"10001","10010","10100","11000","10100","10010","10001"}, {"10000","10000","10000","10000","10000","10000","11111"},
    {"10001","11011","10101","10101","10001","10001","10001"}, {"10001","11001","10101","10011","10001","10001","10001"},
    {"01110","10001","10001","10001","10001","10001","01110"}, {"11110","10001","10001","11110","10000","10000","10000"},
    {"01110","10001","10001","10001","10101","10010","01101"}, {"11110","10001","10001","11110","10100","10010","10001"},
    {"01111","10000","10000","01110","00001","00001","11110"}, {"11111","00100","00100","00100","00100","00100","00100"},
    {"10001","10001","10001","10001","10001","10001","01110"}, {"10001","10001","10001","10001","10001","01010","00100"},
    {"10001","10001","10001","10101","10101","10101","01010"}, {"10001","10001","01010","00100","01010","10001","10001"},
    {"10001","10001","01010","00100","00100","00100","00100"}, {"11111","00001","00010","00100","01000","10000","11111"},
    {"01110","10001","10011","10101","11001","10001","01110"}, {"00100","01100","00100","00100","00100","00100","01110"},
    {"01110","10001","00001","00010","00100","01000","11111"}, {"11110","00001","00001","01110","00001","00001","11110"},
    {"00010","00110","01010","10010","11111","00010","00010"}, {"11111","10000","11110","00001","00001","10001","01110"},
    {"00110","01000","10000","11110","10001","10001","01110"}, {"11111","00001","00010","00100","01000","01000","01000"},
    {"01110","10001","10001","01110","10001","10001","01110"}, {"01110","10001","10001","01111","00001","00010","01100"},
    {"01110","10001","00001","00010","00100","00000","00100"},
};

#define T 64
static int clampc(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static int mixc(int c, int target, int pct) { return (c * (100 - pct) + target * pct) / 100; }

/* 64x64 XPM: rounded NeXT-style tile in the category colour with the app's initial on it. */
static int gen_tile(const char *dest, unsigned rgb, int variant, char letter) {
    int delta = (variant - 2) * 8;
    int r = clampc((int)((rgb >> 16) & 255) + delta), g = clampc((int)((rgb >> 8) & 255) + delta), b = clampc((int)(rgb & 255) + delta);
    static char px[T][T + 1]; static int in[T][T];
    const int R = 7;
    for (int y = 0; y < T; y++) for (int x = 0; x < T; x++) {
        int cx = x < R ? R - 1 : x >= T - R ? T - R : x, cy = y < R ? R - 1 : y >= T - R ? T - R : y;
        int dx = x - cx, dy = y - cy; in[y][x] = dx * dx + dy * dy <= R * R - R;
    }
    for (int y = 0; y < T; y++) {
        for (int x = 0; x < T; x++) {
            char c;
            if (!in[y][x]) c = ' ';
            else if (x == 0 || y == 0 || x == T - 1 || y == T - 1 || !in[y][x-1] || !in[y][x+1] || !in[y-1][x] || !in[y+1][x]) c = 'o';
            else {
                int t = y, l = x, bt = T - 1 - y, rt = T - 1 - x, m = t;
                if (l < m) m = l;
                if (bt < m) m = bt;
                if (rt < m) m = rt;
                c = m > 3 ? '.' : (m == t || m == l) ? 'h' : 'd';
            }
            px[y][x] = c;
        }
        px[y][T] = 0;
    }
    const char *p = strchr(glyph_chars, letter); int gi = p ? (int)(p - glyph_chars) : (int)strlen(glyph_chars) - 1;
    const int S = 6, ox = (T - 5 * S) / 2, oy = (T - 7 * S) / 2;
    for (int pass = 0; pass < 2; pass++) {                           /* shadow, then letter */
        int off = pass == 0 ? 2 : 0; char ch = pass == 0 ? 's' : 'w';
        for (int gy = 0; gy < 7; gy++) for (int gx = 0; gx < 5; gx++) {
            if (font[gi][gy][gx] != '1') continue;
            for (int yy = 0; yy < S; yy++) for (int xx = 0; xx < S; xx++) px[oy + gy * S + yy + off][ox + gx * S + xx + off] = ch;
        }
    }
    char tmp[4700]; snprintf(tmp, sizeof tmp, "%s.%d.tmp.xpm", dest, (int)getpid());
    FILE *o = fopen(tmp, "w"); if (!o) return 0;
    fprintf(o, "/* XPM */\nstatic char *wm_icon[] = {\n\"%d %d 7 1\",\n\"  c None\",\n", T, T);
    fprintf(o, "\". c #%02x%02x%02x\",\n", r, g, b);
    fprintf(o, "\"h c #%02x%02x%02x\",\n", mixc(r, 255, 40), mixc(g, 255, 40), mixc(b, 255, 40));
    fprintf(o, "\"d c #%02x%02x%02x\",\n", mixc(r, 0, 40), mixc(g, 0, 40), mixc(b, 0, 40));
    fprintf(o, "\"o c #%02x%02x%02x\",\n", mixc(r, 0, 70), mixc(g, 0, 70), mixc(b, 0, 70));
    fprintf(o, "\"w c #FFFFFF\",\n\"s c #%02x%02x%02x\",\n", mixc(r, 0, 70), mixc(g, 0, 70), mixc(b, 0, 70));
    for (int y = 0; y < T; y++) fprintf(o, "\"%s\"%s\n", px[y], y + 1 < T ? "," : "");
    fputs("};\n", o);
    if (fclose(o) || rename(tmp, dest)) { unlink(tmp); return 0; }
    return 1;
}

static char initial(const char *name) {
    for (; *name; name++) if ((unsigned char)*name < 128 && isalnum((unsigned char)*name)) return (char)toupper((unsigned char)*name);
    return '?';
}

/* -------------------------------------------------------------- public API */

const char *wmicons_source_name(WMIconSource s) {
    switch (s) {
    case WMICON_THEME: return "theme"; case WMICON_CONVERTED: return "converted";
    case WMICON_CATEGORY: return "category"; case WMICON_GENERATED: return "generated";
    default: return "none";
    }
}

WMIconSource wmicons_for_app(const WMApp *a, int size, char *out, size_t outlen) {
    if (outlen) out[0] = 0;
    if (size <= 0) size = 64;
    /* 1+2: the app's own icon, trying every name it is known by */
    char bin[256] = "", lcls[256] = ""; const char *cand[6]; int nc = 0;
    if (a->icon[0]) cand[nc++] = a->icon;
    if (a->flatpak_id[0]) cand[nc++] = a->flatpak_id;
    cand[nc++] = a->id;
    if (a->wm_class[0]) {
        cand[nc++] = a->wm_class;
        size_t k = 0; for (; a->wm_class[k] && k + 1 < sizeof lcls; k++) lcls[k] = (char)tolower((unsigned char)a->wm_class[k]);
        lcls[k] = 0; if (strcmp(lcls, a->wm_class)) cand[nc++] = lcls;
    }
    if (!a->flatpak_id[0]) { wmapps_bin(a, bin, sizeof bin); if (bin[0]) cand[nc++] = bin; }
    for (int i = 0; i < nc; i++) {
        int dup = 0; for (int j = 0; j < i; j++) if (!strcmp(cand[i], cand[j])) dup = 1;
        Found f; if (dup || !find_icon(cand[i], size, &f)) continue;
        WMIconSource s = pick(&f, size, WMICON_THEME, WMICON_CONVERTED, out, outlen);
        if (s != WMICON_NONE) return s;
    }
    /* 3: the theme's generic icon for the category */
    const CatInfo *ci = cat_lookup(a->category);
    for (int i = 0; i < 4 && ci->names[i]; i++) {
        Found f; if (!find_icon(ci->names[i], size, &f)) continue;
        WMIconSource s = pick(&f, size, WMICON_CATEGORY, WMICON_CATEGORY, out, outlen);
        if (s != WMICON_NONE) return s;
    }
    /* 4: draw one */
    char cd[4096]; if (!cache_dir(cd, sizeof cd)) return WMICON_NONE;
    char L = initial(a->name), dest[4608]; int variant = (int)(hash(a->id) % 5);
    snprintf(dest, sizeof dest, "%s/gen1-%s-%02x-%d.xpm", cd, ci->cat, (unsigned)(unsigned char)L, variant);
    if (!exists(dest) && !gen_tile(dest, ci->rgb, variant, L)) return WMICON_NONE;
    snprintf(out, outlen, "%s", dest); return WMICON_GENERATED;
}
