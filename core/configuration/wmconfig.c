#define _POSIX_C_SOURCE 200809L
#include "wmconfig.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void wmconfig_path(char *out, size_t n) {
    const char *f = getenv("WMCONFIG_FILE"), *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (f && *f) snprintf(out, n, "%s", f);
    else if (x && *x) snprintf(out, n, "%s/wm-desktop/settings.conf", x);
    else snprintf(out, n, "%s/.config/wm-desktop/settings.conf", h ? h : "");
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s); while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
    return s;
}

/* Split "k = v" in place. Returns 0 for blank/comment/malformed lines. */
static int split(char *line, char **k, char **v) {
    char *t = trim(line); if (!*t || *t == '#') return 0;
    char *eq = strchr(t, '='); if (!eq) return 0;
    *eq = 0; *k = trim(t); *v = trim(eq + 1); return **k != 0;
}

const char *wmconfig_get(const char *key, const char *def) {
    static char val[1024]; char p[4096], line[2048];
    wmconfig_path(p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return def;
    const char *res = def;
    while (fgets(line, sizeof line, f)) {                 /* last assignment wins */
        char *k, *v; if (split(line, &k, &v) && !strcmp(k, key)) { snprintf(val, sizeof val, "%s", v); res = val; }
    }
    fclose(f); return res;
}

int wmconfig_set(const char *key, const char *value) {
    char p[4096], tmp[4200], line[2048]; wmconfig_path(p, sizeof p);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", p, (int)getpid());
    FILE *in = fopen(p, "r"), *out = fopen(tmp, "w"); if (!out) { if (in) fclose(in); return -1; }
    int done = 0;
    while (in && fgets(line, sizeof line, in)) {
        char copy[2048]; snprintf(copy, sizeof copy, "%s", line); char *k, *v;
        if (split(copy, &k, &v) && !strcmp(k, key)) { if (!done) fprintf(out, "%s = %s\n", key, value); done = 1; }
        else fputs(line, out);
    }
    if (!done) fprintf(out, "%s = %s\n", key, value);
    if (in) fclose(in);
    if (fclose(out) || rename(tmp, p)) { unlink(tmp); return -1; }
    return 0;
}
