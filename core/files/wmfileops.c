#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "wmfileops.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAXDEPTH 64
static char errbuf[512];
const char *wmfo_error(void) { return errbuf; }
static int fail(const char *what, const char *path, int err) {
    snprintf(errbuf, sizeof errbuf, "%s: %s%s%.200s", what, err ? strerror(err) : "", path ? " - " : "", path ? path : ""); return -1;
}

static int exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }
static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static int join(char *out, size_t n, const char *dir, const char *name) {
    int k = snprintf(out, n, "%s/%s", strcmp(dir, "/") ? dir : "", name); return k > 0 && (size_t)k < n ? 0 : -1;
}

void wmfo_unique(const char *dir, const char *name, char *out, size_t n) {
    char p[PATH_MAX]; if (join(p, sizeof p, dir, name) == 0 && !exists(p)) { snprintf(out, n, "%s", name); return; }
    const char *dot = strrchr(name, '.'); size_t stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    for (int i = 1; i < 10000; i++) {
        char cand[NAME_MAX + 80];
        if (i == 1) snprintf(cand, sizeof cand, "%.*s (copy)%s", (int)stem, name, name + stem);
        else snprintf(cand, sizeof cand, "%.*s (copy %d)%s", (int)stem, name, i, name + stem);
        if (join(p, sizeof p, dir, cand) == 0 && !exists(p)) { snprintf(out, n, "%s", cand); return; }
    }
    snprintf(out, n, "%s", name);
}

static int valid_name(const char *name) {
    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) > NAME_MAX) { snprintf(errbuf, sizeof errbuf, "invalid name"); return 0; }
    return 1;
}

int wmfo_mkdir(const char *parent, const char *name, char *out, size_t n) {
    char p[PATH_MAX]; if (!valid_name(name)) return -1;
    if (join(p, sizeof p, parent, name)) return fail("path too long", NULL, 0);
    if (mkdir(p, 0777)) return fail("cannot create folder", p, errno);
    if (out) { snprintf(out, n, "%s", p); }
    return 0;
}

int wmfo_rename(const char *path, const char *newname, char *out, size_t n) {
    char dir[PATH_MAX], np[PATH_MAX]; if (!valid_name(newname)) return -1;
    snprintf(dir, sizeof dir, "%s", path); char *s = strrchr(dir, '/'); if (!s) return fail("not an absolute path", path, 0);
    if (s == dir) s[1] = 0; else *s = 0;
    if (join(np, sizeof np, dir, newname)) return fail("path too long", NULL, 0);
    if (!strcmp(path, np)) { if (out) { snprintf(out, n, "%s", np); }
    return 0; }
    if (exists(np)) return fail("already exists", np, 0);
    if (rename(path, np)) return fail("cannot rename", path, errno);
    if (out) { snprintf(out, n, "%s", np); }
    return 0;
}

/* ---- recursive removal and copy ---- */
static int rm_tree(const char *p, int depth) {
    struct stat st; if (lstat(p, &st)) return fail("cannot stat", p, errno);
    if (S_ISDIR(st.st_mode)) {
        if (depth > MAXDEPTH) return fail("folder nested too deeply", p, 0);
        DIR *d = opendir(p); if (!d) return fail("cannot read folder", p, errno);
        struct dirent *e; int rc = 0;
        while (!rc && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[PATH_MAX]; if (join(c, sizeof c, p, e->d_name)) { rc = fail("path too long", p, 0); break; }
            rc = rm_tree(c, depth + 1);
        }
        closedir(d); if (rc) return rc;
        return rmdir(p) ? fail("cannot remove folder", p, errno) : 0;
    }
    return unlink(p) ? fail("cannot remove", p, errno) : 0;
}

static int copy_file(const char *src, const char *dst, mode_t mode, const struct stat *st) {
    int in = open(src, O_RDONLY | O_NOFOLLOW), out = -1; if (in < 0) return fail("cannot read", src, errno);
    out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600); if (out < 0) { int e = errno; close(in); return fail("cannot create", dst, e); }
    char buf[65536]; ssize_t k; int rc = 0;
    while ((k = read(in, buf, sizeof buf)) > 0) {
        for (ssize_t off = 0; off < k;) { ssize_t w = write(out, buf + off, (size_t)(k - off)); if (w < 0) { rc = fail("write failed", dst, errno); goto done; } off += w; }
    }
    if (k < 0) rc = fail("read failed", src, errno);
done:
    close(in);
    if (!rc) { fchmod(out, mode & 07777); struct timespec ts[2] = { st->st_atim, st->st_mtim }; futimens(out, ts); }
    if (close(out) && !rc) rc = fail("write failed", dst, errno);
    if (rc) unlink(dst);
    return rc;
}

static int copy_tree(const char *src, const char *dst, int depth) {
    struct stat st; if (lstat(src, &st)) return fail("cannot stat", src, errno);
    if (S_ISLNK(st.st_mode)) {
        char t[PATH_MAX]; ssize_t k = readlink(src, t, sizeof t - 1); if (k < 0) return fail("cannot read link", src, errno);
        t[k] = 0; return symlink(t, dst) ? fail("cannot create link", dst, errno) : 0;
    }
    if (S_ISREG(st.st_mode)) return copy_file(src, dst, st.st_mode, &st);
    if (!S_ISDIR(st.st_mode)) return fail("unsupported file type", src, 0);
    if (depth > MAXDEPTH) return fail("folder nested too deeply", src, 0);
    if (mkdir(dst, 0700)) return fail("cannot create folder", dst, errno);
    DIR *d = opendir(src); if (!d) return fail("cannot read folder", src, errno);
    struct dirent *e; int rc = 0;
    while (!rc && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[PATH_MAX], t[PATH_MAX];
        if (join(s, sizeof s, src, e->d_name) || join(t, sizeof t, dst, e->d_name)) { rc = fail("path too long", src, 0); break; }
        rc = copy_tree(s, t, depth + 1);
    }
    closedir(d);
    if (!rc) { chmod(dst, st.st_mode & 07777); struct timespec ts[2] = { st.st_atim, st.st_mtim }; utimensat(AT_FDCWD, dst, ts, 0); }
    return rc;
}

/* Is dir the same as, or inside, tree? (real paths, so symlinks cannot fool it) */
static int inside(const char *tree, const char *dir) {
    char a[PATH_MAX], b[PATH_MAX]; if (!realpath(tree, a) || !realpath(dir, b)) return 0;
    size_t n = strlen(a); if (!strcmp(a, "/")) return 1;
    return !strncmp(a, b, n) && (b[n] == 0 || b[n] == '/');
}

static int prepare(const char *src, const char *dstdir, char *dst, size_t dn, char *name, size_t nn) {
    struct stat st, ds; if (lstat(src, &st)) return fail("cannot stat", src, errno);
    if (stat(dstdir, &ds) || !S_ISDIR(ds.st_mode)) return fail("destination is not a folder", dstdir, 0);
    if (S_ISDIR(st.st_mode) && inside(src, dstdir)) return fail("cannot put a folder inside itself", src, 0);
    wmfo_unique(dstdir, base(src), name, nn);
    return join(dst, dn, dstdir, name) ? fail("path too long", NULL, 0) : 0;
}

int wmfo_copy(const char *src, const char *dstdir, char *out, size_t n) {
    char dst[PATH_MAX], name[NAME_MAX + 64];
    if (prepare(src, dstdir, dst, sizeof dst, name, sizeof name)) return -1;
    if (copy_tree(src, dst, 0)) { rm_tree(dst, 0); return -1; }       /* no half-copied leftovers */
    if (out) { snprintf(out, n, "%s", dst); }
    return 0;
}

int wmfo_move(const char *src, const char *dstdir, char *out, size_t n) {
    char dst[PATH_MAX], name[NAME_MAX + 64];
    if (prepare(src, dstdir, dst, sizeof dst, name, sizeof name)) return -1;
    if (!rename(src, dst)) { if (out) { snprintf(out, n, "%s", dst); } return 0; }
    if (errno != EXDEV) return fail("cannot move", src, errno);
    if (copy_tree(src, dst, 0)) { rm_tree(dst, 0); return -1; }
    if (rm_tree(src, 0)) return -1;                                  /* copy exists, original stuck: reported */
    if (out) { snprintf(out, n, "%s", dst); }
    return 0;
}

/* ---- trash ---- */
static void pct(const char *s, char *out, size_t n) {
    size_t k = 0;
    for (; *s && k + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("/-_.~", c)) out[k++] = (char)c;
        else k += (size_t)snprintf(out + k, 4, "%%%02X", c);
    }
    out[k] = 0;
}

static int mkpath(const char *path) {
    char b[PATH_MAX]; snprintf(b, sizeof b, "%s", path);
    for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; if (mkdir(b, 0700) && errno != EEXIST) return -1; *s = '/'; }
    return mkdir(b, 0700) && errno != EEXIST ? -1 : 0;
}

int wmfo_trash(const char *path) {
    char abs[PATH_MAX], root[PATH_MAX - 16], files[PATH_MAX], info[PATH_MAX];
    struct stat st; if (lstat(path, &st)) return fail("cannot stat", path, errno);
    if (!realpath(path, abs)) { snprintf(abs, sizeof abs, "%s", path); }          /* dangling symlink: use as is */
    const char *xd = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xd && *xd) snprintf(root, sizeof root, "%.4000s/Trash", xd); else snprintf(root, sizeof root, "%.4000s/.local/share/Trash", home ? home : "");
    snprintf(files, sizeof files, "%s/files", root); snprintf(info, sizeof info, "%s/info", root);
    if (mkpath(files) || mkpath(info)) return fail("cannot create trash", root, errno);
    if (!strcmp(path, "/") || (home && !strcmp(abs, home))) return fail("refusing to trash", path, 0);
    char rtrash[PATH_MAX]; if (realpath(root, rtrash) && S_ISDIR(st.st_mode) && inside(abs, rtrash)) return fail("cannot trash a folder that contains the trash", path, 0);
    char name[NAME_MAX + 64], ipath[PATH_MAX + NAME_MAX + 128], tpath[PATH_MAX + NAME_MAX + 128]; int fd = -1;
    const char *b = base(path); snprintf(name, sizeof name, "%.*s", NAME_MAX - 20, b);
    for (int i = 0; i < 1000; i++) {                                 /* the .trashinfo is created first: that is the reservation */
        char cand[NAME_MAX + 80]; if (i == 0) snprintf(cand, sizeof cand, "%s", name); else snprintf(cand, sizeof cand, "%.*s.%d", NAME_MAX - 20, name, i);
        snprintf(ipath, sizeof ipath, "%s/%s.trashinfo", info, cand); snprintf(tpath, sizeof tpath, "%s/%s", files, cand);
        if (exists(tpath)) continue;
        fd = open(ipath, O_WRONLY | O_CREAT | O_EXCL, 0600); if (fd >= 0) break; if (errno != EEXIST) return fail("cannot write trash info", ipath, errno);
    }
    if (fd < 0) return fail("trash name clash", path, 0);
    char enc[PATH_MAX * 3]; pct(abs, enc, sizeof enc); time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); char date[32]; strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
    dprintf(fd, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, date); close(fd);
    if (!rename(path, tpath)) return 0;
    int e = errno;
    if (e == EXDEV) {
        if (copy_tree(path, tpath, 0) == 0 && rm_tree(path, 0) == 0) return 0;
        rm_tree(tpath, 0); unlink(ipath); return -1;
    }
    unlink(ipath); return fail("cannot move to trash", path, e);
}
