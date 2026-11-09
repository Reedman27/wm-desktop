#define _POSIX_C_SOURCE 200809L
#include "wmtheme.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ---- a small property-list reader: strings, arrays ( a, b ), dictionaries { k = v; } ---- */
typedef struct PL { int type; char *s; struct PL **v; size_t n; } PL;   /* 0 string, 1 array, 2 dict (k0,v0,k1,v1...) */

static void pl_free(PL *p) {
    if (!p) return;
    for (size_t i = 0; i < p->n; i++) pl_free(p->v[i]);
    free(p->v); free(p->s); free(p);
}
static void pl_push(PL *p, PL *c) {
    PL **nv = realloc(p->v, (p->n + 1) * sizeof *nv);
    if (!nv) { pl_free(c); return; }
    p->v = nv; p->v[p->n++] = c;
}
static PL *pl_new(int type) { PL *p = calloc(1, sizeof *p); if (p) p->type = type; return p; }

static const char *skip(const char *s) {
    for (;;) {
        while (isspace((unsigned char)*s)) s++;
        if (s[0] == '/' && s[1] == '/') { while (*s && *s != '\n') s++; }
        else if (s[0] == '/' && s[1] == '*') { s += 2; while (*s && !(s[0] == '*' && s[1] == '/')) s++; if (*s) s += 2; }
        else return s;
    }
}
static int bare_char(char c) { return c && !isspace((unsigned char)c) && !strchr("{}()=,;\"", c); }

static PL *parse_value(const char **sp, int depth) {
    const char *s = skip(*sp);
    if (depth > 32 || !*s) return NULL;
    PL *p = NULL;
    if (*s == '{' || *s == '(') {
        int dict = *s == '{'; char close = dict ? '}' : ')';
        p = pl_new(dict ? 2 : 1); if (!p) return NULL; s++;
        for (;;) {
            s = skip(s);
            if (*s == close) { s++; break; }
            if (!*s) { pl_free(p); return NULL; }
            PL *a = parse_value(&s, depth + 1); if (!a) { pl_free(p); return NULL; }
            pl_push(p, a);
            s = skip(s);
            if (dict) {
                if (*s != '=') { pl_free(p); return NULL; }
                s = skip(s + 1);
                PL *b = parse_value(&s, depth + 1); if (!b) { pl_free(p); return NULL; }
                pl_push(p, b); s = skip(s);
                if (*s == ';') s++; else if (*s != close) { pl_free(p); return NULL; }
            } else {
                if (*s == ',') s++; else if (*s != close) { pl_free(p); return NULL; }
            }
        }
    } else if (*s == '"') {
        size_t cap = 32, n = 0; char *b = malloc(cap); if (!b) return NULL; s++;
        while (*s && *s != '"') {
            char c = *s++;
            if (c == '\\' && *s) { c = *s++; if (c == 'n') c = '\n'; else if (c == 't') c = '\t'; }
            if (n + 2 > cap) { cap *= 2; char *nb = realloc(b, cap); if (!nb) { free(b); return NULL; } b = nb; }
            b[n++] = c;
        }
        if (*s != '"') { free(b); return NULL; }
        s++; b[n] = 0; p = pl_new(0); if (!p) { free(b); return NULL; } p->s = b;
    } else if (bare_char(*s)) {
        const char *e = s; while (bare_char(*e)) e++;
        p = pl_new(0); if (!p) return NULL; p->s = strndup(s, (size_t)(e - s)); s = e;
    } else return NULL;
    *sp = s; return p;
}

static PL *pl_read_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    size_t cap = 8192, n = 0; char *b = malloc(cap);
    for (size_t k; b && (k = fread(b + n, 1, cap - n - 1, f)) > 0;) { n += k; if (n + 1 >= cap) { cap *= 2; b = realloc(b, cap); } }
    fclose(f); if (!b) return NULL; b[n] = 0;
    const char *s = b; PL *p = parse_value(&s, 0); free(b);
    if (p && p->type != 2) { pl_free(p); p = NULL; }
    return p;
}
static const PL *pl_get(const PL *d, const char *key) {
    if (!d || d->type != 2) return NULL;
    for (size_t i = 0; i + 1 < d->n; i += 2) if (d->v[i]->type == 0 && !strcmp(d->v[i]->s, key)) return d->v[i + 1];
    return NULL;
}

/* ---- texture / colour / font extraction ---- */
static void put(char *dst, size_t n, const char *s) { snprintf(dst, n, "%s", s); }

static void take_color(const PL *d, const char *key, char *dst, size_t n) {
    const PL *v = pl_get(d, key);
    if (v && v->type == 0 && v->s[0]) put(dst, n, v->s);
}
static void take_font(const PL *d, const char *key, char *dst, size_t n) {
    const PL *v = pl_get(d, key);
    if (v && v->type == 0 && v->s[0]) put(dst, n, v->s);
}
/* (solid, c) (xgradient, c1, c2 ...) (tpixmap, file, c) or a bare colour string -> one colour */
static void take_texture(const PL *d, const char *key, char *dst, size_t n) {
    const PL *v = pl_get(d, key);
    if (!v) return;
    if (v->type == 0) { if (v->s[0]) put(dst, n, v->s); return; }
    if (v->type != 1 || v->n < 2 || v->v[0]->type != 0) return;
    const char *kind = v->v[0]->s; size_t k = strlen(kind);
    int pix = k >= 6 && !strcasecmp(kind + k - 6, "pixmap");
    const PL *c = v->v[pix ? (v->n > 2 ? 2 : 1) : 1];
    if (pix && v->n < 3) return;                           /* pixmap without a colour: keep the default */
    if (c->type == 0 && c->s[0]) put(dst, n, c->s);
}

static void apply(WMTheme *t, const PL *d) {
    take_color(d, "MenuTextColor", t->menu_text_color, sizeof t->menu_text_color);
    take_color(d, "MenuTitleColor", t->menu_title_color, sizeof t->menu_title_color);
    take_color(d, "MenuDisabledColor", t->menu_disabled_color, sizeof t->menu_disabled_color);
    take_color(d, "HighlightColor", t->highlight_color, sizeof t->highlight_color);
    take_color(d, "HighlightTextColor", t->highlight_text_color, sizeof t->highlight_text_color);
    take_color(d, "FrameBorderColor", t->frame_border_color, sizeof t->frame_border_color);
    take_texture(d, "MenuTextBack", t->menu_text_back, sizeof t->menu_text_back);
    take_texture(d, "MenuTitleBack", t->menu_title_back, sizeof t->menu_title_back);
    take_font(d, "MenuTextFont", t->menu_text_font, sizeof t->menu_text_font);
    take_font(d, "MenuTitleFont", t->menu_title_font, sizeof t->menu_title_font);
}

void wmtheme_defaults(WMTheme *t) {                        /* values from Window Maker's src/defaults.c and wconfig.h */
    memset(t, 0, sizeof *t);
    put(t->menu_text_color, sizeof t->menu_text_color, "black");
    put(t->menu_title_color, sizeof t->menu_title_color, "white");
    put(t->menu_disabled_color, sizeof t->menu_disabled_color, "gray50");
    put(t->highlight_color, sizeof t->highlight_color, "white");
    put(t->highlight_text_color, sizeof t->highlight_text_color, "black");
    put(t->frame_border_color, sizeof t->frame_border_color, "black");
    put(t->menu_text_back, sizeof t->menu_text_back, "rgb:aa/aa/aa");
    put(t->menu_title_back, sizeof t->menu_title_back, "black");
    put(t->menu_text_font, sizeof t->menu_text_font, "Sans:pixelsize=12");
    put(t->menu_title_font, sizeof t->menu_title_font, "Sans:bold:pixelsize=12");
}

int wmtheme_defaults_path(const char *domain, char *out, size_t n) {
    const char *root = getenv("WMAKER_USER_ROOT"), *home = getenv("HOME");      /* same order as Window Maker's wusergnusteppath() */
    if (root && *root) {
        if (root[0] == '~' && (root[1] == '/' || !root[1]) && home) snprintf(out, n, "%s%s/Defaults/%s", home, root + 1, domain);
        else snprintf(out, n, "%s/Defaults/%s", root, domain);
    } else if (home && *home) snprintf(out, n, "%s/GNUstep/Defaults/%s", home, domain);
    else return 0;
    return 1;
}
int wmtheme_user_path(char *out, size_t n) { return wmtheme_defaults_path("WindowMaker", out, n); }

long wmtheme_mtime(void) {
    char p[4096]; struct stat st;
    return wmtheme_user_path(p, sizeof p) && !stat(p, &st) ? (long)st.st_mtime : 0;
}

static int find_global(char *out, size_t n) {
    const char *e = getenv("WMTHEME_GLOBAL"), *cands[] = { "/etc/WindowMaker/WindowMaker", "/usr/local/etc/WindowMaker/WindowMaker", NULL };
    struct stat st;
    if (e && *e) { snprintf(out, n, "%s", e); return !stat(out, &st); }
    for (int i = 0; cands[i]; i++) if (!stat(cands[i], &st)) { snprintf(out, n, "%s", cands[i]); return 1; }
    return 0;
}

int wmtheme_load(WMTheme *t, const char *user, const char *global) {
    wmtheme_defaults(t);
    char gp[4096], up[4096];
    if (!global) { if (find_global(gp, sizeof gp)) global = gp; }
    if (!user) { if (wmtheme_user_path(up, sizeof up)) user = up; }
    const char *files[2] = { global, user };               /* later files override earlier ones */
    for (int i = 0; i < 2; i++) {
        if (!files[i]) continue;
        PL *d = pl_read_file(files[i]); if (!d) continue;
        apply(t, d); pl_free(d); t->files_read++;
    }
    return t->files_read;
}

/* ---- colours ---- */
static int hexv(const char *s, int n, unsigned *out) {     /* n hex digits -> 8-bit value (scaled if n != 2) */
    unsigned v = 0;
    for (int i = 0; i < n; i++) { if (!isxdigit((unsigned char)s[i])) return 0; v = v * 16 + (unsigned)(isdigit((unsigned char)s[i]) ? s[i] - '0' : tolower((unsigned char)s[i]) - 'a' + 10); }
    *out = n == 1 ? v * 17 : n == 2 ? v : n == 3 ? v >> 4 : v >> 8; return 1;
}
int wmtheme_color(const char *s, unsigned *rgb) {
    if (!s) return 0;
    unsigned r, g, b;
    if (s[0] == '#') {
        size_t n = strlen(s + 1);
        if (n == 3 || n == 6 || n == 9 || n == 12) {
            int w = (int)n / 3;
            if (hexv(s + 1, w, &r) && hexv(s + 1 + w, w, &g) && hexv(s + 1 + 2 * w, w, &b)) { *rgb = r << 16 | g << 8 | b; return 1; }
        }
        return 0;
    }
    if (!strncasecmp(s, "rgb:", 4)) {
        const char *p = s + 4; unsigned c[3];
        for (int i = 0; i < 3; i++) {
            const char *e = p; while (isxdigit((unsigned char)*e)) e++;
            int w = (int)(e - p); if (w < 1 || w > 4 || !hexv(p, w, &c[i])) return 0;
            if (i < 2) { if (*e != '/') return 0; p = e + 1; } else if (*e) return 0;
        }
        *rgb = c[0] << 16 | c[1] << 8 | c[2]; return 1;
    }
    static const struct { const char *n; unsigned v; } names[] = {
        { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x00FF00 }, { "blue", 0x0000FF },
        { "yellow", 0xFFFF00 }, { "cyan", 0x00FFFF }, { "magenta", 0xFF00FF }, { "gray", 0xBEBEBE }, { "grey", 0xBEBEBE },
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcasecmp(s, names[i].n)) { *rgb = names[i].v; return 1; }
    if (!strncasecmp(s, "gray", 4) || !strncasecmp(s, "grey", 4)) {      /* grayN, N in 0..100 */
        char *e; long v = strtol(s + 4, &e, 10);
        if (e != s + 4 && !*e && v >= 0 && v <= 100) { unsigned c = (unsigned)((v * 255 + 50) / 100); *rgb = c << 16 | c << 8 | c; return 1; }
    }
    return 0;
}

static unsigned mix(unsigned c, unsigned target, int pct) {
    unsigned out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        unsigned a = (c >> sh) & 255, b = (target >> sh) & 255;
        out |= ((a * (unsigned)(100 - pct) + b * (unsigned)pct) / 100) << sh;
    }
    return out;
}
void wmtheme_shades(unsigned face, unsigned *light, unsigned *dark) {
    if (light) *light = mix(face, 0xFFFFFF, 55);
    if (dark) *dark = mix(face, 0x000000, 50);
}

/* ---- fonts ---- */
static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

int wmtheme_font_patterns(const char *spec, char out[][160], int max) {
    int n = 0;
    if (!spec) return 0;
    char *copy = strdup(spec), *save = NULL; if (!copy) return 0;
    for (char *item = strtok_r(copy, ",", &save); item && n < max; item = strtok_r(NULL, ",", &save)) {
        while (isspace((unsigned char)*item)) item++;
        if (!*item) continue;
        if (*item == '-') { snprintf(out[n++], 160, "%s", item); continue; }   /* already an XLFD */
        char *parts = strdup(item), *ps = NULL; if (!parts) continue;
        char *family = strtok_r(parts, ":", &ps); char famlow[64] = ""; int bold = 0, italic = 0, px = 0; double pt = 0;
        if (family) { snprintf(famlow, sizeof famlow, "%s", family); lower(famlow); }
        for (char *a; (a = strtok_r(NULL, ":", &ps));) {
            char al[64]; snprintf(al, sizeof al, "%s", a); lower(al);
            if (!strcmp(al, "bold") || !strcmp(al, "demibold") || !strcmp(al, "black")) bold = 1;
            else if (!strcmp(al, "italic") || !strcmp(al, "oblique")) italic = 1;
            else if (!strncmp(al, "pixelsize=", 10)) px = atoi(al + 10);
            else if (!strncmp(al, "size=", 5)) pt = atof(al + 5);
            else if (!strncmp(al, "weight=", 7)) { if (strstr(al + 7, "bold") || strstr(al + 7, "black") || !strcmp(al + 7, "200")) bold = 1; }
            else if (!strncmp(al, "slant=", 6)) { if (strstr(al + 6, "italic") || strstr(al + 6, "oblique") || !strcmp(al + 6, "100")) italic = 1; }
        }
        if (!px && pt > 0) px = (int)(pt * 96.0 / 72.0 + 0.5);
        if (px <= 0) px = 12;
        const char *w = bold ? "bold" : "medium"; char sl = italic ? 'i' : 'r';
        const char *fams[4]; int nf = 0;                    /* what a core-font server calls the usual aliases */
        if (!strcmp(famlow, "sans") || !strcmp(famlow, "sans-serif") || !strcmp(famlow, "sans serif") || !strcmp(famlow, "helvetica") || !strcmp(famlow, "arial")) {
            fams[nf++] = "helvetica"; fams[nf++] = "arial"; fams[nf++] = "lucida"; }
        else if (!strcmp(famlow, "serif") || !strcmp(famlow, "times") || !strcmp(famlow, "times new roman")) { fams[nf++] = "times"; fams[nf++] = "*"; }
        else if (!strcmp(famlow, "mono") || !strcmp(famlow, "monospace") || !strcmp(famlow, "courier")) { fams[nf++] = "courier"; fams[nf++] = "fixed"; }
        else { static char own[64]; snprintf(own, sizeof own, "%s", famlow); fams[nf++] = own; }
        for (int i = 0; i < nf && n < max; i++)
            snprintf(out[n++], 160, "-*-%s-%s-%c-*-*-%d-*-*-*-*-*-*-*", fams[i], w, sl, px);
        if (n < max) snprintf(out[n++], 160, "-*-*-%s-%c-*-*-%d-*-*-*-*-*-iso8859-1", w, sl, px);   /* any family, right weight/size */
        free(parts);
    }
    free(copy); return n;
}
