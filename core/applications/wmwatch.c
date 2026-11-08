#define _POSIX_C_SOURCE 200809L
#include "wmwatch.h"
#include "wmapps.h"
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_DEPTH 8
#define TREE_MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF)
#define PARENT_MASK (IN_CREATE | IN_MOVED_TO)   /* only to notice applications/ appearing */

struct WMWatch { int fd; char **dirs; size_t ndirs; int *wds; size_t nwd, cap; };

static int known(const WMWatch *w, int wd) { for (size_t i = 0; i < w->nwd; i++) if (w->wds[i] == wd) return 1; return 0; }

static int add(WMWatch *w, const char *path, uint32_t mask) {
    int wd = inotify_add_watch(w->fd, path, mask);
    if (wd < 0 || known(w, wd)) return 0;
    if (w->nwd == w->cap) { w->cap = w->cap ? w->cap * 2 : 32; w->wds = realloc(w->wds, w->cap * sizeof *w->wds); if (!w->wds) abort(); }
    w->wds[w->nwd++] = wd; return 1;
}

static int add_tree(WMWatch *w, const char *dir, int depth) {
    int n = add(w, dir, TREE_MASK);
    if (depth >= MAX_DEPTH) return n;
    DIR *d = opendir(dir); if (!d) return n;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[8192]; snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        struct stat st; if (!stat(sub, &st) && S_ISDIR(st.st_mode)) n += add_tree(w, sub, depth + 1);
    }
    closedir(d); return n;
}

int wmwatch_rearm(WMWatch *w) {
    int n = 0;
    for (size_t i = 0; i < w->ndirs; i++) {
        char app[4096]; snprintf(app, sizeof app, "%s/applications", w->dirs[i]);
        struct stat st;
        if (!stat(app, &st) && S_ISDIR(st.st_mode)) n += add_tree(w, app, 0);
        else add(w, w->dirs[i], PARENT_MASK);        /* watch the parent until applications/ shows up */
    }
    return n;
}

WMWatch *wmwatch_open(const char *extra_dir) {
    WMWatch *w = calloc(1, sizeof *w); if (!w) abort();
    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd < 0) { free(w); return NULL; }
    w->ndirs = wmapps_dirs(&w->dirs, extra_dir);
    wmwatch_rearm(w);
    return w;
}

int wmwatch_fd(const WMWatch *w) { return w->fd; }
int wmwatch_count(const WMWatch *w) { return (int)w->nwd; }

int wmwatch_poll(WMWatch *w) {
    char buf[16384] __attribute__((aligned(8))); int changed = 0; ssize_t n;
    while ((n = read(w->fd, buf, sizeof buf)) > 0) {
        for (char *p = buf; p < buf + n;) {
            const struct inotify_event *ev = (const struct inotify_event *)p;
            p += sizeof *ev + ev->len;
            if (ev->mask & IN_Q_OVERFLOW) { changed = 1; continue; }
            if (ev->mask & IN_IGNORED) {                    /* watched dir deleted/unmounted */
                for (size_t i = 0; i < w->nwd; i++) if (w->wds[i] == ev->wd) { w->wds[i] = w->wds[--w->nwd]; break; }
                changed = 1; continue;
            }
            if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) { changed = 1; continue; }
            if (!ev->len) continue;
            size_t l = strlen(ev->name);
            if (ev->mask & IN_ISDIR) changed = 1;
            else if (l > 8 && !strcmp(ev->name + l - 8, ".desktop")) changed = 1;
            else if (!strcmp(ev->name, "applications")) changed = 1;
        }
    }
    if (changed) wmwatch_rearm(w);       /* new subdirectories / applications/ trees */
    return changed;
}

void wmwatch_close(WMWatch *w) {
    if (!w) return;
    close(w->fd); wmapps_dirs_free(w->dirs, w->ndirs); free(w->wds); free(w);
}
