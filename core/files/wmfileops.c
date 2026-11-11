#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "wmfileops.h"
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAXDEPTH 64
static char errbuf[512];
const char *wmfo_error(void) { return errbuf; }
static int fail(const char *what, const char *path, int err) {
    snprintf(errbuf, sizeof errbuf, "%s: %s%s%.200s", what, err ? strerror(err) : "", path ? " - " : "", path ? path : ""); return -1;
}

static int exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }
/* rename(2) that never replaces a destination which appeared after our own name check.
 * Linux renameat2(RENAME_NOREPLACE) does this atomically. Where the kernel or the filesystem
 * lacks it (ENOSYS, EINVAL, ENOTSUP) we fall back without losing the guarantee that matters:
 *   - non-directories: link() + unlink(), and link() fails with EEXIST instead of replacing;
 *   - directories: plain rename(), which can only replace an EMPTY directory, so no data can be lost. */
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif
static int rename_noreplace(const char *src, const char *dst) {
#if defined(SYS_renameat2) && !defined(WMFO_NO_RENAMEAT2)    /* WMFO_NO_RENAMEAT2: tests force the fallback */
    if (syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst, RENAME_NOREPLACE) == 0) return 0;
    if (errno != ENOSYS && errno != EINVAL && errno != ENOTSUP && errno != EOPNOTSUPP) return -1;
#endif
    struct stat st;
    if (lstat(src, &st)) return -1;
    if (S_ISDIR(st.st_mode)) return rename(src, dst);
    if (link(src, dst)) return -1;               /* EEXIST if something is there; EXDEV is handled by callers */
    if (unlink(src)) { int e = errno; unlink(dst); errno = e; return -1; }
    return 0;
}

/* Resolve the parent directory, not the final component: the final component may
 * itself be a symlink (including a dangling symlink) that must be trashed as-is. */
static int absolute_entry_path(const char *path, char *out, size_t n) {
    char input[PATH_MAX], parent[PATH_MAX], resolved[PATH_MAX];
    if (!path || !*path) { errno = EINVAL; return -1; }
    if (path[0] == '/') {
        if (snprintf(input, sizeof input, "%s", path) >= (int)sizeof input) { errno = ENAMETOOLONG; return -1; }
    } else {
        if (!getcwd(parent, sizeof parent)) return -1;
        if (snprintf(input, sizeof input, "%s/%s", parent, path) >= (int)sizeof input) { errno = ENAMETOOLONG; return -1; }
    }
    char *slash = strrchr(input, '/');
    if (!slash || !slash[1]) { errno = EINVAL; return -1; }
    char name[NAME_MAX + 1];
    if (strlen(slash + 1) > NAME_MAX) { errno = ENAMETOOLONG; return -1; }
    snprintf(name, sizeof name, "%s", slash + 1);
    if (!strcmp(name, ".") || !strcmp(name, "..")) { errno = EINVAL; return -1; }
    if (slash == input) slash[1] = 0; else *slash = 0;
    if (!realpath(input, resolved)) return -1;
    int k = snprintf(out, n, "%s%s%s", !strcmp(resolved, "/") ? "" : resolved, "/", name);
    if (k < 0 || (size_t)k >= n) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
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
    if (rename_noreplace(path, np)) return fail(errno == EEXIST ? "already exists" : "cannot rename", path, errno);
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

static int copy_tree(const char *src, const char *dst, int depth, int *created) {
    if (created) *created = 0;
    struct stat st; if (lstat(src, &st)) return fail("cannot stat", src, errno);
    if (S_ISLNK(st.st_mode)) {
        char t[PATH_MAX]; ssize_t k = readlink(src, t, sizeof t - 1); if (k < 0) return fail("cannot read link", src, errno);
        t[k] = 0;
        if (symlink(t, dst)) return fail("cannot create link", dst, errno);
        if (created) *created = 1;
        return 0;
    }
    if (S_ISREG(st.st_mode)) {
        int rc = copy_file(src, dst, st.st_mode, &st);
        if (!rc && created) *created = 1;
        return rc;
    }
    if (!S_ISDIR(st.st_mode)) return fail("unsupported file type", src, 0);
    if (depth > MAXDEPTH) return fail("folder nested too deeply", src, 0);
    if (mkdir(dst, 0700)) return fail("cannot create folder", dst, errno);
    if (created) *created = 1;
    DIR *d = opendir(src); if (!d) return fail("cannot read folder", src, errno);
    struct dirent *e; int rc = 0;
    while (!rc && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[PATH_MAX], t[PATH_MAX];
        if (join(s, sizeof s, src, e->d_name) || join(t, sizeof t, dst, e->d_name)) { rc = fail("path too long", src, 0); break; }
        rc = copy_tree(s, t, depth + 1, NULL);
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
    int created = 0;
    if (copy_tree(src, dst, 0, &created)) { if (created) rm_tree(dst, 0); return -1; } /* remove only our partial copy */
    if (out) { snprintf(out, n, "%s", dst); }
    return 0;
}

int wmfo_move(const char *src, const char *dstdir, char *out, size_t n) {
    char dst[PATH_MAX], name[NAME_MAX + 64];
    if (prepare(src, dstdir, dst, sizeof dst, name, sizeof name)) return -1;
    if (!rename_noreplace(src, dst)) { if (out) { snprintf(out, n, "%s", dst); } return 0; }
    if (errno != EXDEV) return fail("cannot move", src, errno);
    int created = 0;
    if (copy_tree(src, dst, 0, &created)) { if (created) rm_tree(dst, 0); return -1; }
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

int wmfo_trash_to(const char *path, char *out, size_t on) {
    char abs[PATH_MAX], root[PATH_MAX - 16], files[PATH_MAX], info[PATH_MAX];
    struct stat st; if (lstat(path, &st)) return fail("cannot stat", path, errno);
    if (absolute_entry_path(path, abs, sizeof abs)) return fail("cannot resolve parent of", path, errno);
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
    if (!rename_noreplace(path, tpath)) { if (out) snprintf(out, on, "%s", tpath); return 0; }
    int e = errno;
    if (e == EXDEV) {
        int created = 0;
        if (copy_tree(path, tpath, 0, &created)) {
            if (created) rm_tree(tpath, 0);
            unlink(ipath);
            return -1;
        }
        if (rm_tree(path, 0)) return -1; /* keep the complete trash copy and metadata if source removal is partial */
        if (out) snprintf(out, on, "%s", tpath);
        return 0;
    }
    unlink(ipath); return fail("cannot move to trash", path, e);
}

int wmfo_trash(const char *path) { return wmfo_trash_to(path, NULL, 0); }

static int unpct(const char *s, char *out, size_t n) {
    size_t k = 0;
    while (*s && k + 1 < n) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) { char h[3] = { s[1], s[2], 0 }; out[k++] = (char)strtol(h, NULL, 16); s += 3; }
        else out[k++] = *s++;
    }
    out[k] = 0; return 0;
}

/* Puts a file that wmfo_trash_to() returned back where it came from (a free name if that one is taken). */
int wmfo_restore(const char *trashed, char *out, size_t n) {
    char dir[PATH_MAX], info[PATH_MAX + NAME_MAX + 32], line[PATH_MAX * 3], orig[PATH_MAX] = "";
    snprintf(dir, sizeof dir, "%s", trashed); char *sl = strrchr(dir, '/'); if (!sl) return fail("not a trash path", trashed, 0);
    *sl = 0; sl = strrchr(dir, '/'); if (!sl || strcmp(sl, "/files")) return fail("not inside a Trash/files folder", trashed, 0);
    *sl = 0; snprintf(info, sizeof info, "%s/info/%s.trashinfo", dir, base(trashed));
    FILE *f = fopen(info, "r"); if (!f) return fail("no .trashinfo for", trashed, errno);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "Path=", 5)) { line[strcspn(line, "\r\n")] = 0; unpct(line + 5, orig, sizeof orig); }
    fclose(f);
    if (orig[0] != '/') return fail("the .trashinfo has no usable Path for", trashed, 0);
    char pdir[PATH_MAX]; snprintf(pdir, sizeof pdir, "%s", orig); sl = strrchr(pdir, '/'); *(sl == pdir ? sl + 1 : sl) = 0;
    struct stat st; if (stat(pdir, &st) || !S_ISDIR(st.st_mode)) return fail("the original folder is gone:", pdir, 0);
    char name[NAME_MAX + 64], dst[PATH_MAX * 2]; wmfo_unique(pdir, base(orig), name, sizeof name);
    snprintf(dst, sizeof dst, "%s/%s", strcmp(pdir, "/") ? pdir : "", name);
    if (rename_noreplace(trashed, dst)) {
        if (errno != EXDEV) return fail(errno == EEXIST ? "restore destination already exists" : "cannot restore", trashed, errno);
        int created = 0;
        if (copy_tree(trashed, dst, 0, &created)) { if (created) rm_tree(dst, 0); return -1; }
        if (rm_tree(trashed, 0)) return -1; /* keep the complete restored copy; never erase user data */
    }
    unlink(info); if (out) snprintf(out, n, "%s", dst);
    return 0;
}
