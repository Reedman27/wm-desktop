#define _POSIX_C_SOURCE 200809L
/* wmsettings - one settings file, applied to whichever backend is running.
 *
 *   wmsettings list                  every key with its current value
 *   wmsettings get KEY | set KEY VAL
 *   wmsettings apply [--dry-run]
 *
 * Translated to Window Maker defaults through Window Maker's own 'wdwrite' tool. */
#include "../../core/configuration/wmconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    const char *key, *def, *choices;     /* choices: "a|b|c", or "int:LO-HI", or "" for free text */
    const char *wm_key;                  /* Window Maker defaults key, NULL = consumed by wm-desktop itself */
    const char *wm_vals;                 /* for choice keys: WM value per choice, same order, '|' separated */
} Setting;

static const Setting S[] = {
    { "focus",     "click", "click|sloppy", "FocusMode", "Manual|Sloppy" },
    { "placement", "auto",  "auto|smart|cascade|random|manual|center", "WindowPlacement", "Auto|Smart|Cascade|Random|Manual|Center" },
    { "icon_size", "64",    "int:24-128", "IconSize", NULL },
    { "animations","on",    "on|off", "DisableAnimations", "NO|YES" },
    { "dock",      "on",    "on|off", "DisableDock", "NO|YES" },
    { "terminal",  "xterm -e", "", NULL, NULL },          /* used by wmmenud for Terminal=true apps */
};
#define NS (sizeof S / sizeof *S)

static const Setting *find(const char *k) { for (size_t i = 0; i < NS; i++) if (!strcmp(S[i].key, k)) return &S[i]; return NULL; }

static int valid(const Setting *s, const char *v) {
    if (!*s->choices) return *v != 0;
    if (!strncmp(s->choices, "int:", 4)) {
        int lo, hi; char *end; sscanf(s->choices + 4, "%d-%d", &lo, &hi);
        long n = strtol(v, &end, 10); return *v && !*end && n >= lo && n <= hi;
    }
    char c[256]; snprintf(c, sizeof c, "%s", s->choices);
    for (char *t = strtok(c, "|"); t; t = strtok(NULL, "|")) if (!strcmp(t, v)) return 1;
    return 0;
}

static const char *translate(const Setting *s, const char *v, char *buf, size_t n) {
    if (!s->wm_vals) return v;
    char c[256], w[256]; snprintf(c, sizeof c, "%s", s->choices); snprintf(w, sizeof w, "%s", s->wm_vals);
    char *sc, *sw; char *a = strtok_r(c, "|", &sc), *b = strtok_r(w, "|", &sw);
    for (; a && b; a = strtok_r(NULL, "|", &sc), b = strtok_r(NULL, "|", &sw))
        if (!strcmp(a, v)) { snprintf(buf, n, "%s", b); return buf; }
    return v;
}

int main(int argc, char **argv) {
    if (argc < 2) { fputs("usage: wmsettings list|get KEY|set KEY VALUE|apply [--dry-run]\n", stderr); return 2; }
    if (!strcmp(argv[1], "list")) {
        for (size_t i = 0; i < NS; i++) printf("%-10s = %-12s  (%s)\n", S[i].key, wmconfig_get(S[i].key, S[i].def), *S[i].choices ? S[i].choices : "text");
    } else if (!strcmp(argv[1], "get") && argc == 3) {
        const Setting *s = find(argv[2]); if (!s) { fprintf(stderr, "wmsettings: unknown key %s\n", argv[2]); return 1; }
        puts(wmconfig_get(s->key, s->def));
    } else if (!strcmp(argv[1], "set") && argc == 4) {
        const Setting *s = find(argv[2]); if (!s) { fprintf(stderr, "wmsettings: unknown key %s\n", argv[2]); return 1; }
        if (!valid(s, argv[3])) { fprintf(stderr, "wmsettings: %s must be %s\n", s->key, !*s->choices ? "non-empty" : !strncmp(s->choices, "int:", 4) ? s->choices + 4 : s->choices); return 1; }
        return wmconfig_set(s->key, argv[3]) ? (perror("wmsettings"), 1) : 0;
    } else if (!strcmp(argv[1], "apply")) {
        int dry = 0;
        for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--dry-run")) dry = 1;
        int rc = 0;
        for (size_t i = 0; i < NS; i++) {
            if (!S[i].wm_key) continue;
            char buf[64]; const char *v = translate(&S[i], wmconfig_get(S[i].key, S[i].def), buf, sizeof buf);
            if (dry) { printf("wdwrite WindowMaker %s %s\n", S[i].wm_key, v); continue; }
            pid_t p = fork();
            if (p == 0) { execlp("wdwrite", "wdwrite", "WindowMaker", S[i].wm_key, v, (char *)NULL); _exit(127); }
            int st; waitpid(p, &st, 0);
            if (!WIFEXITED(st) || WEXITSTATUS(st)) { fprintf(stderr, "wmsettings: wdwrite failed for %s (is Window Maker's wdwrite installed?)\n", S[i].wm_key); rc = 1; }
        }
        return rc;
    } else { fputs("usage: wmsettings list|get KEY|set KEY VALUE|apply [--dry-run]\n", stderr); return 2; }
    return 0;
}
