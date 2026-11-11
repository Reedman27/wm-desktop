#define _POSIX_C_SOURCE 200809L
#include "wmapps.h"
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

#define MAX_DEPTH 8     /* nested applications/ subdirectories; also bounds symlink loops */

const char *const wmapps_category_names[] = {
    "Accessories", "Development", "Education", "Games", "Graphics",
    "Internet", "Multimedia", "Office", "Settings", "System", "Other", NULL
};

static const struct { const char *xdg, *wm; } cat_map[] = {
    /* first match wins; ordered so specific categories beat generic ones */
    {"Game", "Games"}, {"Development", "Development"}, {"IDE", "Development"},
    {"Graphics", "Graphics"}, {"Office", "Office"},
    {"Network", "Internet"}, {"WebBrowser", "Internet"}, {"Email", "Internet"},
    {"AudioVideo", "Multimedia"}, {"Audio", "Multimedia"}, {"Video", "Multimedia"},
    {"Education", "Education"}, {"Science", "Education"},
    {"Settings", "Settings"}, {"System", "System"}, {"TerminalEmulator", "System"},
    {"Utility", "Accessories"}, {"Accessibility", "Accessories"},
    {NULL, NULL}
};

/* ------------------------------------------------------------------ helpers */

static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) abort(); return p; }
static char *xstrdup(const char *s) { char *d = strdup(s ? s : ""); if (!d) abort(); return d; }

typedef struct { char **v; size_t n, cap; } Strs;
static void strs_push(Strs *s, char *owned) {
    if (s->n + 1 >= s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->v = xrealloc(s->v, s->cap * sizeof *s->v); }
    s->v[s->n++] = owned; s->v[s->n] = NULL;
}
static int strs_has(const Strs *s, const char *x) {
    for (size_t i = 0; i < s->n; i++) if (!strcmp(s->v[i], x)) return 1;
    return 0;
}
static void strs_free(Strs *s) { for (size_t i = 0; i < s->n; i++) free(s->v[i]); free(s->v); memset(s, 0, sizeof *s); }

typedef struct { char *b; size_t n, cap; } Buf;
static void buf_add(Buf *b, const char *s, size_t len) {
    if (b->n + len + 1 > b->cap) { b->cap = (b->n + len + 1) * 2; b->b = xrealloc(b->b, b->cap); }
    memcpy(b->b + b->n, s, len); b->n += len; b->b[b->n] = 0;
}
static void buf_ch(Buf *b, char c) { buf_add(b, &c, 1); }
static char *buf_take(Buf *b) { char *r = b->b ? b->b : xstrdup(""); memset(b, 0, sizeof *b); return r; }

static void trim_end(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n] = 0;
}

/* Desktop Entry string escapes: \s \n \t \r \\ (anything else: backslash dropped, char kept). */
static char *unescape_value(const char *v) {
    char *out = xstrdup(v), *w = out;
    for (const char *r = v; *r; r++) {
        if (*r == '\\' && r[1]) {
            r++;
            *w++ = *r=='s' ? ' ' : *r=='n' ? '\n' : *r=='t' ? '\t' : *r=='r' ? '\r' : *r;
        } else *w++ = *r;
    }
    *w = 0; return out;
}

/* Is 'item' in a ';' separated list (backslash-escaped ';' inside items is not a separator). */
static int in_list(const char *list, const char *item) {
    size_t n = strlen(item);
    for (const char *p = list; p && *p;) {
        const char *e = p;
        while (*e && *e != ';') { if (*e == '\\' && e[1]) e++; e++; }
        if ((size_t)(e - p) == n && !strncmp(p, item, n)) return 1;
        if (!*e) break;
        p = e + 1;
    }
    return 0;
}

static int in_desktops(const char *list, const char *desktops /* colon separated */) {
    char *d = xstrdup(desktops), *save = d; int hit = 0;
    for (char *s = d, *e; ; s = e + 1) {
        e = strchr(s, ':'); if (e) *e = 0;
        if (*s && in_list(list, s)) hit = 1;
        if (!e) break;
    }
    free(save); return hit;
}

static const char *pick_category(const char *cats) {
    for (int i = 0; cat_map[i].xdg; i++)
        if (in_list(cats, cat_map[i].xdg)) return cat_map[i].wm;
    return "Other";
}

/* --------------------------------------------------------- locale matching */

/* Spec order for lang_COUNTRY@MODIFIER: lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER, lang. */
typedef struct { char c[4][300]; } Loc;   /* 3 * 95 + separators fits: no truncation possible */

static void loc_init(Loc *l) {
    memset(l, 0, sizeof *l);
    const char *v = getenv("LC_ALL"); if (!v || !*v) v = getenv("LC_MESSAGES"); if (!v || !*v) v = getenv("LANG");
    if (!v || !*v) return;
    char t[96]; snprintf(t, sizeof t, "%s", v);
    char *mod = strchr(t, '@'); if (mod) *mod++ = 0;
    char *dot = strchr(t, '.'); if (dot) *dot = 0;
    char *country = strchr(t, '_'); if (country) *country++ = 0;
    if (!t[0] || !strcmp(t, "C") || !strcmp(t, "POSIX")) return;
    if (country && mod) snprintf(l->c[0], sizeof l->c[0], "%s_%s@%s", t, country, mod);
    if (country)        snprintf(l->c[1], sizeof l->c[1], "%s_%s", t, country);
    if (mod)            snprintf(l->c[2], sizeof l->c[2], "%s@%s", t, mod);
    snprintf(l->c[3], sizeof l->c[3], "%s", t);
}

typedef struct { char *v; int rank; } LStr;   /* rank: 1 = unlocalized, 2..5 = locale matches */

static void lstr_offer(LStr *s, const char *key, const char *base, const char *raw, const Loc *loc) {
    size_t bl = strlen(base);
    if (strncmp(key, base, bl)) return;
    int rank = 0;
    if (!key[bl]) rank = 1;
    else if (key[bl] == '[') {
        const char *q = key + bl + 1, *e = strchr(q, ']');
        if (e && !e[1])
            for (int i = 0; i < 4; i++)
                if (loc->c[i][0] && strlen(loc->c[i]) == (size_t)(e - q) && !strncmp(q, loc->c[i], (size_t)(e - q))) { rank = 5 - i; break; }
    }
    if (rank > s->rank) { free(s->v); s->v = unescape_value(raw); s->rank = rank; }
}

/* --------------------------------------------------------- Exec handling */

void wmapps_argv_free(char **argv) {
    if (!argv) return;
    for (size_t i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

int wmapps_tokenize(const char *s, char ***out, size_t *outn) {
    Strs v = {0}; Buf b = {0}; int in_tok = 0; char q = 0;
    for (const char *p = s; *p; p++) {
        if (q == '\'') {
            if (*p == '\'') q = 0; else buf_ch(&b, *p);
        } else if (q == '"') {
            if (*p == '"') q = 0;
            else if (*p == '\\' && p[1] && strchr("\"\\$`", p[1])) buf_ch(&b, *++p);
            else buf_ch(&b, *p);
        } else if (*p == ' ' || *p == '\t' || *p == '\n') {
            if (in_tok) { strs_push(&v, buf_take(&b)); in_tok = 0; }
        } else if (*p == '"' || *p == '\'') { q = *p; in_tok = 1; }
        else if (*p == '\\') {
            if (!p[1]) { free(b.b); strs_free(&v); return -1; }
            buf_ch(&b, *++p); in_tok = 1;
        } else { buf_ch(&b, *p); in_tok = 1; }
    }
    if (q) { free(b.b); strs_free(&v); return -1; }
    if (in_tok) strs_push(&v, buf_take(&b));
    if (!v.v) v.v = xrealloc(NULL, sizeof *v.v), v.v[0] = NULL;
    *out = v.v; *outn = v.n; return 0;
}

char *wmapps_shell_quote(const char *s) {
    int plain = *s != 0;
    for (const char *p = s; *p; p++)
        if (!(isalnum((unsigned char)*p) || strchr("_@+=:,./-", *p))) { plain = 0; break; }
    if (plain) return xstrdup(s);
    Buf b = {0}; buf_ch(&b, '\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'') buf_add(&b, "'\\''", 4); else buf_ch(&b, *p);
    }
    buf_ch(&b, '\''); return buf_take(&b);
}

static int is_scheme_uri(const char *s) {
    if (!isalpha((unsigned char)*s)) return 0;
    const char *p = s + 1;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
    return *p == ':' && p[1];
}

static int hexv(int c) { return isdigit(c) ? c - '0' : (tolower(c) >= 'a' && tolower(c) <= 'f') ? tolower(c) - 'a' + 10 : -1; }

/* %f / %F: a local path. file:// URIs are decoded; other schemes cannot be passed. */
static char *arg_as_path(const char *f) {
    if (strncasecmp(f, "file://", 7)) return is_scheme_uri(f) ? NULL : xstrdup(f);
    const char *p = f + 7;
    if (*p != '/') { const char *slash = strchr(p, '/'); if (!slash) return NULL; p = slash; }   /* skip host */
    Buf b = {0};
    for (; *p; p++) {
        if (*p == '%' && hexv((unsigned char)p[1]) >= 0 && hexv((unsigned char)p[2]) >= 0) { buf_ch(&b, (char)(hexv((unsigned char)p[1]) * 16 + hexv((unsigned char)p[2]))); p += 2; }
        else buf_ch(&b, *p);
    }
    return buf_take(&b);
}

/* %u / %U: a URI. Plain paths become file:// URIs. */
static char *arg_as_uri(const char *f) {
    if (is_scheme_uri(f)) return xstrdup(f);
    Buf b = {0}; buf_add(&b, "file://", 7);
    char cwd[4096];
    if (f[0] != '/' && getcwd(cwd, sizeof cwd)) { for (const char *c = cwd; *c; c++) { char t[4]; if (isalnum((unsigned char)*c) || strchr("-._~/", *c)) buf_ch(&b, *c); else { snprintf(t, sizeof t, "%%%02X", (unsigned char)*c); buf_add(&b, t, 3); } } buf_ch(&b, '/'); }
    for (const char *c = f; *c; c++) {
        char t[4];
        if (isalnum((unsigned char)*c) || strchr("-._~/", *c)) buf_ch(&b, *c);
        else { snprintf(t, sizeof t, "%%%02X", (unsigned char)*c); buf_add(&b, t, 3); }
    }
    return buf_take(&b);
}

int wmapps_accepts_files(const WMApp *a) {
    int kind = 0;
    for (size_t i = 0; i < a->argc; i++)
        for (const char *p = a->argv[i]; *p; p++) {
            if (*p != '%' || !p[1]) continue;
            p++;
            if (*p == 'F' || *p == 'U') return 2;
            if (*p == 'f' || *p == 'u') kind = 1;
        }
    return kind;
}

char **wmapps_expand(const WMApp *a, const char *const *files, size_t nfiles, size_t *outn) {
    Strs v = {0};
    for (size_t i = 0; i < a->argc; i++) {
        const char *t = a->argv[i];
        if (!strcmp(t, "%F") || !strcmp(t, "%U")) {                 /* whole-token list: one arg per file */
            for (size_t k = 0; k < nfiles; k++) {
                char *x = t[1] == 'F' ? arg_as_path(files[k]) : arg_as_uri(files[k]);
                if (x) strs_push(&v, x);
            }
            continue;
        }
        if (!strcmp(t, "%i")) {                                     /* two args */
            if (a->icon[0]) { strs_push(&v, xstrdup("--icon")); strs_push(&v, xstrdup(a->icon)); }
            continue;
        }
        Buf b = {0}; int had_code = 0;
        for (const char *p = t; *p; p++) {
            if (*p != '%' || !p[1]) { buf_ch(&b, *p); continue; }
            p++;
            switch (*p) {
            case '%': buf_ch(&b, '%'); break;
            case 'f': case 'F': case 'u': case 'U': {
                had_code = 1;
                if (nfiles) {
                    char *x = (*p == 'f' || *p == 'F') ? arg_as_path(files[0]) : arg_as_uri(files[0]);
                    if (x) { buf_add(&b, x, strlen(x)); free(x); }
                }
                break; }
            case 'c': had_code = 1; buf_add(&b, a->name, strlen(a->name)); break;
            case 'k': had_code = 1; buf_add(&b, a->path, strlen(a->path)); break;
            default:  had_code = 1; break;       /* %i embedded, deprecated %d %D %n %N %v %m, unknown: removed */
            }
        }
        if (had_code && !b.n) { free(b.b); continue; }               /* token consisted only of codes that expanded to nothing */
        strs_push(&v, buf_take(&b));
    }
    if (!v.v) { v.v = xrealloc(NULL, sizeof *v.v); v.v[0] = NULL; }
    *outn = v.n; return v.v;
}

char *wmapps_shell_command(const WMApp *a, const char *const *files, size_t nfiles, const char *terminal) {
    size_t n; char **argv = wmapps_expand(a, files, nfiles, &n);
    Buf b = {0};
    if (terminal && a->terminal) {
        char **t = NULL; size_t tn = 0;
        if (wmapps_tokenize(terminal, &t, &tn) || !tn) {
            wmapps_argv_free(t);
            t = NULL; tn = 0;
            if (wmapps_tokenize("xterm -e", &t, &tn)) {
                wmapps_argv_free(argv);
                free(b.b);
                return NULL;
            }
        }
        for (size_t i = 0; i < tn; i++) { char *q = wmapps_shell_quote(t[i]); if (b.n) buf_ch(&b, ' '); buf_add(&b, q, strlen(q)); free(q); }
        wmapps_argv_free(t);
    }
    for (size_t i = 0; i < n; i++) { char *q = wmapps_shell_quote(argv[i]); if (b.n) buf_ch(&b, ' '); buf_add(&b, q, strlen(q)); free(q); }
    wmapps_argv_free(argv);
    return buf_take(&b);
}

static int spawn(char *const *argv, const char *cwd, char *err, size_t errlen) {
    int p[2];
    if (pipe(p)) { snprintf(err, errlen, "pipe: %s", strerror(errno)); return -1; }
    fcntl(p[0], F_SETFD, FD_CLOEXEC); fcntl(p[1], F_SETFD, FD_CLOEXEC);
    pid_t c = fork();
    if (c < 0) { snprintf(err, errlen, "fork: %s", strerror(errno)); close(p[0]); close(p[1]); return -1; }
    if (c == 0) {                               /* intermediate: detach, fork the real child, vanish */
        close(p[0]); setsid();
        pid_t g = fork();
        if (g != 0) { int e = errno; if (g < 0 && write(p[1], &e, sizeof e)) {} _exit(g < 0 ? 127 : 0); }
        if (cwd && *cwd && chdir(cwd)) { int e = errno; if (write(p[1], &e, sizeof e)) {} _exit(127); }
        execvp(argv[0], argv);
        int e = errno; if (write(p[1], &e, sizeof e)) {} _exit(127);
    }
    close(p[1]);
    int st; while (waitpid(c, &st, 0) < 0 && errno == EINTR) {}
    int e = 0; ssize_t k; do k = read(p[0], &e, sizeof e); while (k < 0 && errno == EINTR);
    close(p[0]);
    if (k == (ssize_t)sizeof e) { snprintf(err, errlen, "%s: %s", argv[0], strerror(e)); return -1; }
    return 0;
}

int wmapps_launch(const WMApp *a, const char *const *files, size_t nfiles, const char *terminal, char *err, size_t errlen) {
    if (errlen) err[0] = 0;
    int kind = wmapps_accepts_files(a);
    if (nfiles && !kind) { snprintf(err, errlen, "%s does not accept files", a->name); return -1; }
    size_t runs = (nfiles > 1 && kind == 1) ? nfiles : 1;
    for (size_t r = 0; r < runs; r++) {
        const char *const *fp = files; size_t fn = nfiles;
        if (runs > 1) { fp = files + r; fn = 1; }
        size_t n; char **argv = wmapps_expand(a, fp, fn, &n), **tv = NULL; size_t tn = 0;
        if (terminal && a->terminal) {
            if (wmapps_tokenize(terminal, &tv, &tn) || !tn) { wmapps_argv_free(tv); wmapps_tokenize("xterm -e", &tv, &tn); }
            char **all = xrealloc(NULL, (tn + n + 1) * sizeof *all);
            for (size_t i = 0; i < tn; i++) all[i] = tv[i];
            for (size_t i = 0; i < n; i++) all[tn + i] = argv[i];
            all[tn + n] = NULL; free(argv); free(tv); argv = all; n += tn;
        }
        if (!n) { snprintf(err, errlen, "%s: empty command", a->name); wmapps_argv_free(argv); return -1; }
        int rc = spawn(argv, a->work_dir, err, errlen);
        wmapps_argv_free(argv);
        if (rc) return -1;
    }
    return 0;
}

/* ------------------------------------------------------------- parsing */

typedef struct {
    Strs claimed;           /* desktop-file IDs already taken by a higher-priority file */
    const char *desktops;
    Loc loc;
} Scan;

enum { E_NOT_ENTRY = 0, E_CLAIMED = 1, E_SHOWN = 2 };

static int exec_exists(const char *tryexec) {
    if (strchr(tryexec, '/')) return access(tryexec, X_OK) == 0;
    const char *p = getenv("PATH"); int ok = 0;
    char *d = xstrdup(p ? p : "/usr/local/bin:/usr/bin:/bin");
    for (char *s = d, *e; !ok; s = e + 1) {
        e = strchr(s, ':'); if (e) *e = 0;
        char full[4096]; snprintf(full, sizeof full, "%s/%s", *s ? s : ".", tryexec);
        if (access(full, X_OK) == 0) ok = 1;
        if (!e) break;
    }
    free(d); return ok;
}

static void note_skipped(WMAppRegistry *r, const char *path, const char *why) {
    r->skipped = xrealloc(r->skipped, (r->nskipped + 1) * sizeof *r->skipped);
    size_t n = strlen(path) + strlen(why) + 3; char *m = xrealloc(NULL, n);
    snprintf(m, n, "%s: %s", path, why); r->skipped[r->nskipped++] = m;
}

static int load_entry(Scan *sc, WMAppRegistry *reg, WMApp *a, const char *path, const char *id, WMAppOrigin origin) {
    FILE *f = fopen(path, "r"); if (!f) return E_NOT_ENTRY;
    LStr name = {0}, gname = {0}, comment = {0};
    char *line = NULL, *exec = NULL, *icon = NULL, *cls = NULL, *cats = NULL, *only = NULL, *notin = NULL,
         *tryexec = NULL, *type = NULL, *mime = NULL, *wdir = NULL, *flat = NULL;
    size_t cap = 0; ssize_t len;
    int in_de = 0, seen_de = 0, nodisplay = 0, hidden = 0, term = 0;
#define SETSTR(p) do { free(p); p = unescape_value(v); } while (0)
    while ((len = getline(&line, &cap, f)) >= 0) {
        trim_end(line);
        const char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { in_de = !seen_de && !strcmp(s, "[Desktop Entry]"); if (in_de) seen_de = 1; continue; }
        if (!in_de || *s == '#' || !*s) continue;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char *k = line; while (*k == ' ' || *k == '\t') k++; trim_end(k);
        const char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
        if (!strncmp(k, "Name", 4)) lstr_offer(&name, k, "Name", v, &sc->loc);
        if (!strncmp(k, "GenericName", 11)) lstr_offer(&gname, k, "GenericName", v, &sc->loc);
        if (!strncmp(k, "Comment", 7)) lstr_offer(&comment, k, "Comment", v, &sc->loc);
        if (!strcmp(k, "Type")) SETSTR(type);
        else if (!strcmp(k, "Exec")) SETSTR(exec);
        else if (!strcmp(k, "TryExec")) SETSTR(tryexec);
        else if (!strcmp(k, "Icon")) SETSTR(icon);
        else if (!strcmp(k, "Path")) SETSTR(wdir);
        else if (!strcmp(k, "StartupWMClass")) SETSTR(cls);
        else if (!strcmp(k, "X-Flatpak")) SETSTR(flat);
        else if (!strcmp(k, "Categories")) { free(cats); cats = xstrdup(v); }
        else if (!strcmp(k, "MimeType")) { free(mime); mime = xstrdup(v); }
        else if (!strcmp(k, "OnlyShowIn")) { free(only); only = xstrdup(v); }
        else if (!strcmp(k, "NotShowIn")) { free(notin); notin = xstrdup(v); }
        else if (!strcmp(k, "NoDisplay")) nodisplay = !strcmp(v, "true");
        else if (!strcmp(k, "Hidden")) hidden = !strcmp(v, "true");
        else if (!strcmp(k, "Terminal")) term = !strcmp(v, "true");
    }
    free(line); fclose(f);

    int result = seen_de ? E_CLAIMED : E_NOT_ENTRY;     /* any desktop entry claims its ID, shown or not */
    char **argv = NULL; size_t argc = 0;
    int ok = seen_de && !hidden && !nodisplay;
    if (ok && (!type || strcmp(type, "Application"))) {
        if (!type) note_skipped(reg, path, "missing Type");
        ok = 0;
    }
    if (ok && (!name.v || !*name.v)) { note_skipped(reg, path, "missing Name"); ok = 0; }
    if (ok && (!exec || !*exec)) { note_skipped(reg, path, "missing Exec"); ok = 0; }
    if (ok && wmapps_tokenize(exec, &argv, &argc)) { note_skipped(reg, path, "malformed Exec (unbalanced quote or trailing backslash)"); argv = NULL; ok = 0; }
    if (ok && !argc) { note_skipped(reg, path, "empty Exec"); ok = 0; }
    if (ok && only && !in_desktops(only, sc->desktops)) ok = 0;
    if (ok && notin && in_desktops(notin, sc->desktops)) ok = 0;
    if (ok && tryexec && *tryexec && !exec_exists(tryexec)) ok = 0;   /* uninstalled app */

    if (ok) {
        memset(a, 0, sizeof *a);
        a->id = xstrdup(id); a->path = xstrdup(path); a->name = name.v; name.v = NULL;
        a->generic_name = gname.v ? gname.v : xstrdup(""); gname.v = NULL;
        a->comment = comment.v ? comment.v : xstrdup(""); comment.v = NULL;
        a->argv = argv; a->argc = argc;
        a->icon = icon ? icon : xstrdup(""); icon = NULL;
        a->wm_class = cls ? cls : xstrdup(""); cls = NULL;
        a->category = xstrdup(pick_category(cats ? cats : ""));
        a->mime_types = mime ? mime : xstrdup(""); mime = NULL;
        a->work_dir = wdir ? wdir : xstrdup(""); wdir = NULL;
        a->flatpak_id = flat ? flat : xstrdup(""); flat = NULL;
        a->terminal = term;
        a->origin = (a->flatpak_id[0] || origin == WMAPP_FLATPAK) ? WMAPP_FLATPAK : WMAPP_NATIVE;
        a->exec = wmapps_shell_command(a, NULL, 0, NULL);
        result = E_SHOWN;
    } else wmapps_argv_free(argv);
    free(name.v); free(gname.v); free(comment.v); free(exec); free(icon); free(cls); free(cats);
    free(only); free(notin); free(tryexec); free(type); free(mime); free(wdir); free(flat);
    return result;
}

static int cmp_name(const struct dirent **a, const struct dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }
static int not_dot(const struct dirent *e) { return e->d_name[0] != '.'; }

/* 'sub' is the path below applications/; 'prefix' the ID prefix ("vendor-" for sub "vendor"). */
static void scan_dir(Scan *sc, WMAppRegistry *r, const char *base, WMAppOrigin origin,
                     const char *sub, const char *prefix, int depth) {
    char dir[4096]; snprintf(dir, sizeof dir, "%s/applications%s%s", base, *sub ? "/" : "", sub);
    struct dirent **list; int n = scandir(dir, &list, not_dot, cmp_name);   /* sorted: deterministic precedence */
    if (n < 0) return;
    for (int i = 0; i < n; i++) {
        const char *nm = list[i]->d_name;
        char path[8192]; snprintf(path, sizeof path, "%s/%s", dir, nm);
        struct stat st; if (stat(path, &st)) continue;                      /* follows symlinks (Flatpak exports) */
        if (S_ISDIR(st.st_mode)) {
            if (depth >= MAX_DEPTH) continue;
            char sp[2048], np[2048];
            snprintf(sp, sizeof sp, "%s%s%s", sub, *sub ? "/" : "", nm);
            snprintf(np, sizeof np, "%s%s-", prefix, nm);
            scan_dir(sc, r, base, origin, sp, np, depth + 1);
            continue;
        }
        size_t l = strlen(nm);
        if (l < 9 || strcmp(nm + l - 8, ".desktop")) continue;
        char id[2048]; snprintf(id, sizeof id, "%s%.*s", prefix, (int)(l - 8), nm);
        if (strs_has(&sc->claimed, id)) continue;                           /* earlier dir (or earlier file) wins */
        WMApp a; int rc = load_entry(sc, r, &a, path, id, origin);
        if (rc == E_NOT_ENTRY) continue;
        strs_push(&sc->claimed, xstrdup(id));
        if (rc != E_SHOWN) continue;
        if (r->count == r->cap) { r->cap = r->cap ? r->cap * 2 : 64; r->apps = xrealloc(r->apps, r->cap * sizeof *r->apps); }
        r->apps[r->count++] = a;
    }
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
}

size_t wmapps_dirs(char ***out, const char *extra_dir) {
    const char *home = getenv("HOME"), *xh = getenv("XDG_DATA_HOME"), *xd = getenv("XDG_DATA_DIRS");
    Strs v = {0}; char buf[4096];
#define ADD(str) do { char *c_ = xstrdup(str); size_t l_ = strlen(c_); while (l_ > 1 && c_[l_-1] == '/') c_[--l_] = 0; \
                      if (*c_ && !strs_has(&v, c_)) strs_push(&v, c_); else free(c_); } while (0)
    if (extra_dir) ADD(extra_dir);
    if (xh && *xh) snprintf(buf, sizeof buf, "%s", xh); else snprintf(buf, sizeof buf, "%s/.local/share", home ? home : "");
    ADD(buf);
    /* Flatpak exports are only in XDG_DATA_DIRS if the distro's profile.d ran; a bare
     * Window Maker session often misses them, so add them explicitly. */
    snprintf(buf, sizeof buf, "%s/.local/share/flatpak/exports/share", home ? home : ""); ADD(buf);
    ADD("/var/lib/flatpak/exports/share");
    char *dirs = xstrdup(xd && *xd ? xd : "/usr/local/share:/usr/share");
    for (char *s = dirs, *e; ; s = e + 1) {
        e = strchr(s, ':'); if (e) *e = 0;
        if (*s) ADD(s);
        if (!e) break;
    }
    free(dirs);
    if (!v.v) v.v = xrealloc(NULL, sizeof *v.v);
    *out = v.v; return v.n;
}

void wmapps_dirs_free(char **v, size_t n) { for (size_t i = 0; i < n; i++) free(v[i]); free(v); }

void wmapps_scan(WMAppRegistry *r, const char *desktop, const char *extra_dir) {
    memset(r, 0, sizeof *r);
    Scan sc; memset(&sc, 0, sizeof sc);
    if (!desktop) { desktop = getenv("XDG_CURRENT_DESKTOP"); if (!desktop || !*desktop) desktop = "WindowMaker"; }
    sc.desktops = desktop; loc_init(&sc.loc);
    char **dirs; size_t n = wmapps_dirs(&dirs, extra_dir);
    for (size_t i = 0; i < n; i++)
        scan_dir(&sc, r, dirs[i], strstr(dirs[i], "flatpak/exports") ? WMAPP_FLATPAK : WMAPP_NATIVE, "", "", 0);
    wmapps_dirs_free(dirs, n);
    strs_free(&sc.claimed);
}

/* ------------------------------------------------------------- lookup */

const WMApp *wmapps_find(const WMAppRegistry *r, const char *key) {
    if (!key || !*key) return NULL;
    for (size_t i = 0; i < r->count; i++) if (!strcmp(r->apps[i].id, key)) return &r->apps[i];
    for (size_t i = 0; i < r->count; i++) if (!strcasecmp(r->apps[i].id, key)) return &r->apps[i];
    for (size_t i = 0; i < r->count; i++)
        if (r->apps[i].wm_class[0] && !strcasecmp(r->apps[i].wm_class, key)) return &r->apps[i];
    return NULL;
}

const char *wmapps_bin(const WMApp *a, char *buf, size_t n) {
    buf[0] = 0;
    if (!a->argc) return buf;
    const char *s = strrchr(a->argv[0], '/'); s = s ? s + 1 : a->argv[0];
    snprintf(buf, n, "%s", s); return buf;
}

const WMApp *wmapps_match_window(const WMAppRegistry *r, const char *instance, const char *cls) {
    const WMApp *a;
    if (cls && *cls && (a = wmapps_find(r, cls))) return a;
    if (instance && *instance && (a = wmapps_find(r, instance))) return a;
    for (size_t i = 0; i < r->count; i++) {
        const WMApp *c = &r->apps[i]; char b[256];
        const char *tail = strrchr(c->id, '.'); tail = tail ? tail + 1 : c->id;
        if (c->flatpak_id[0]) b[0] = 0; else wmapps_bin(c, b, sizeof b);   /* "flatpak run ..." says nothing about the window */
        if ((instance && *instance && ((b[0] && !strcasecmp(b, instance)) || !strcasecmp(tail, instance))) ||
            (cls && *cls && ((b[0] && !strcasecmp(b, cls)) || !strcasecmp(tail, cls)))) return c;
    }
    return NULL;
}

void wmapps_free(WMAppRegistry *r) {
    for (size_t i = 0; i < r->count; i++) {
        WMApp *a = &r->apps[i];
        free(a->id); free(a->path); free(a->name); free(a->generic_name); free(a->comment); free(a->exec);
        wmapps_argv_free(a->argv); free(a->icon); free(a->wm_class); free(a->category);
        free(a->mime_types); free(a->work_dir); free(a->flatpak_id);
    }
    for (size_t i = 0; i < r->nskipped; i++) free(r->skipped[i]);
    free(r->skipped); free(r->apps); memset(r, 0, sizeof *r);
}
