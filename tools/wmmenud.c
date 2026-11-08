#define _POSIX_C_SOURCE 200809L
/* wmmenud - keeps a Window Maker (or wlmaker) root menu in sync with installed apps.
 *
 * Watches every applications/ tree known to WMApps (recursively, via WMWatch), debounces
 * bursts (a package install touches many files), regenerates the menu and atomically
 * replaces the output file only if its content changed. Window Maker re-reads the
 * WMRootMenu file when its mtime advances, so no signal or restart is needed.
 *
 *   wmmenud [-o out] [-b x11|wayland] [-t "term -e"] [-T template] [--once] [--dir D] */
#include "../core/applications/wmapps.h"
#include "../core/applications/wmmenu.h"
#include "../core/applications/wmwatch.h"
#include "../core/configuration/wmconfig.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEBOUNCE_MS 750
#define REARM_MS 60000       /* look for data dirs that appeared without us noticing (new Flatpak install) */

static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    size_t cap = 4096, n = 0; char *b = malloc(cap);
    for (size_t k; b && (k = fread(b + n, 1, cap - n - 1, f)) > 0;) {
        n += k; if (n + 1 >= cap) { cap *= 2; b = realloc(b, cap); }
    }
    fclose(f); if (!b) return NULL;
    b[n] = 0; if (len) *len = n; return b;
}

static void mkdirs(char *path) {            /* mkdir -p for the parent of 'path' */
    for (char *p = path + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
}

/* returns 1 if the file was rewritten */
static int regenerate(const char *out, WMMenuBackend be, const char *term, const char *tmpl_path,
                      const char *dir, char **last) {
    WMAppRegistry r; wmapps_scan(&r, NULL, dir);
    char *tmpl = tmpl_path ? slurp(tmpl_path, NULL) : NULL;
    char *buf = NULL; size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    wmmenu_emit_root(m, &r, be, term, tmpl);
    fclose(m); free(tmpl); wmapps_free(&r);
    if (*last && !strcmp(*last, buf)) { free(buf); return 0; }
    char tmp[4096]; snprintf(tmp, sizeof tmp, "%s.tmp.%d", out, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f || fputs(buf, f) == EOF || fclose(f) || rename(tmp, out)) {
        fprintf(stderr, "wmmenud: cannot write %s: %s\n", out, strerror(errno)); unlink(tmp); free(buf); return 0;
    }
    free(*last); *last = buf; return 1;
}

int main(int argc, char **argv) {
    char out[4096] = "", *term = NULL, *tmpl = NULL, *dir = NULL; int once = 0;
    WMMenuBackend be = WMMENU_X11;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) snprintf(out, sizeof out, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) be = !strcmp(argv[++i], "wayland") ? WMMENU_WAYLAND : WMMENU_X11;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) term = argv[++i];
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) tmpl = argv[++i];
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--once")) once = 1;
        else { fprintf(stderr, "usage: wmmenud [-o out] [-b x11|wayland] [-t \"term -e\"] [-T template] [--dir D] [--once]\n"); return 2; }
    }
    if (!out[0]) {
        const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
        if (x && *x) snprintf(out, sizeof out, "%s/wm-desktop/root.menu", x);
        else if (h) snprintf(out, sizeof out, "%s/.config/wm-desktop/root.menu", h);
        else { fputs("wmmenud: no HOME and no -o\n", stderr); return 1; }
    }
    char termbuf[512]; if (!term) { snprintf(termbuf, sizeof termbuf, "%s", wmconfig_get("terminal", "xterm -e")); term = termbuf; }
    mkdirs(out);
    char *last = NULL;
    if (regenerate(out, be, term, tmpl, dir, &last)) fprintf(stderr, "wmmenud: wrote %s\n", out);
    if (once) { free(last); return 0; }

    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sig;   /* no SA_RESTART: poll() must wake */
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    WMWatch *w = wmwatch_open(dir);
    if (!w) { perror("inotify"); return 1; }
    fprintf(stderr, "wmmenud: watching %d directories\n", wmwatch_count(w));

    int pending = 0;
    while (!quit) {
        struct pollfd p = { wmwatch_fd(w), POLLIN, 0 };
        int rc = poll(&p, 1, pending ? DEBOUNCE_MS : REARM_MS);
        if (rc < 0 && errno != EINTR) break;
        if (rc > 0) { if (wmwatch_poll(w)) pending = 1; }
        else if (rc == 0 && pending) {
            pending = 0;
            if (regenerate(out, be, term, tmpl, dir, &last)) fprintf(stderr, "wmmenud: menu updated\n");
        } else if (rc == 0 && wmwatch_rearm(w) > 0) pending = 1;     /* a data dir appeared */
    }
    wmwatch_close(w); free(last);
    return 0;
}
