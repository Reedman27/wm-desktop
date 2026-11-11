#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
/* Unit tests for WMFileOps in throw-away directories. */
#include "../core/files/wmfileops.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s  [%s]\n", __FILE__, __LINE__, #c, wmfo_error()); } } while (0)
static char root[200];
static void put(const char *rel, const char *text) { char p[600]; snprintf(p, sizeof p, "%s/%s", root, rel); FILE *f = fopen(p, "w"); if (!f) { perror(p); exit(2); } fputs(text, f); fclose(f); }
static int has(const char *rel) { char p[600]; struct stat st; snprintf(p, sizeof p, "%s/%s", root, rel); return lstat(p, &st) == 0; }
static char *slurp(const char *rel) { static char b[4096]; char p[600]; snprintf(p, sizeof p, "%s/%s", root, rel); FILE *f = fopen(p, "r"); if (!f) return NULL; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); return b; }
static char *P(const char *rel) { static char b[8][700]; static int i; i = (i + 1) % 8; snprintf(b[i], sizeof b[i], "%s/%s", root, rel); return b[i]; }
static void sh(const char *fmt, const char *a) { char c[1000]; snprintf(c, sizeof c, fmt, a); if (system(c)) {} }

int main(void) {
    snprintf(root, sizeof root, "/tmp/wmfo-test-XXXXXX"); if (!mkdtemp(root)) return 2;
    char home[300]; snprintf(home, sizeof home, "%s/home", root); mkdir(home, 0755); setenv("HOME", home, 1); unsetenv("XDG_DATA_HOME");
    char out[700], uni[300];

    /* unique names */
    put("a.txt", "A"); wmfo_unique(root, "a.txt", uni, sizeof uni); CHECK(!strcmp(uni, "a (copy).txt"));
    put("a (copy).txt", "A"); wmfo_unique(root, "a.txt", uni, sizeof uni); CHECK(!strcmp(uni, "a (copy 2).txt"));
    wmfo_unique(root, "fresh", uni, sizeof uni); CHECK(!strcmp(uni, "fresh"));
    put(".hidden", "h"); wmfo_unique(root, ".hidden", uni, sizeof uni); CHECK(!strcmp(uni, ".hidden (copy)"));    /* dotfile: no fake extension */

    /* mkdir + rename */
    CHECK(wmfo_mkdir(root, "New", out, sizeof out) == 0 && has("New"));
    CHECK(wmfo_mkdir(root, "bad/name", NULL, 0) == -1);  CHECK(wmfo_mkdir(root, "", NULL, 0) == -1);  CHECK(wmfo_mkdir(root, "..", NULL, 0) == -1);
    CHECK(wmfo_mkdir(root, "New", NULL, 0) == -1);                                            /* exists */
    CHECK(wmfo_rename(P("a.txt"), "b.txt", out, sizeof out) == 0 && has("b.txt") && !has("a.txt"));
    CHECK(wmfo_rename(P("b.txt"), "New", NULL, 0) == -1 && has("b.txt"));                    /* never replaces */
    put("keep.txt", "do not replace");
    CHECK(wmfo_rename(P("b.txt"), "keep.txt", NULL, 0) == -1 && !strcmp(slurp("keep.txt"), "do not replace"));
    CHECK(wmfo_rename(P("b.txt"), "../x", NULL, 0) == -1);
    CHECK(wmfo_rename(P("b.txt"), "b.txt", NULL, 0) == 0);                                     /* no-op */

    /* copy: file, tree, symlink, clash, into itself */
    { char c[2000]; snprintf(c, sizeof c, "mkdir -p '%s/tree/sub' && echo deep > '%s/tree/sub/f' && ln -s f '%s/tree/sub/link' && chmod 640 '%s/tree/sub/f'", root, root, root, root); if (system(c)) {} }
    CHECK(wmfo_copy(P("tree"), P("New"), out, sizeof out) == 0);
    CHECK(has("New/tree/sub/f") && has("New/tree/sub/link"));
    { char l[100]; ssize_t k = readlink(P("New/tree/sub/link"), l, sizeof l - 1); CHECK(k == 1 && l[0] == 'f'); }       /* link copied as a link */
    { struct stat st; stat(P("New/tree/sub/f"), &st); CHECK((st.st_mode & 0777) == 0640); }                                /* mode kept */
    CHECK(wmfo_copy(P("tree"), P("New"), out, sizeof out) == 0 && strstr(out, "tree (copy)") && has("New/tree (copy)/sub/f"));   /* clash renamed */
    CHECK(wmfo_copy(P("tree"), P("tree/sub"), NULL, 0) == -1);                                                              /* into itself */
    CHECK(wmfo_copy(P("tree"), P("tree"), NULL, 0) == -1);
    CHECK(wmfo_copy(P("nope"), P("New"), NULL, 0) == -1);   CHECK(wmfo_copy(P("b.txt"), P("b.txt"), NULL, 0) == -1);       /* missing src, file as destination */
    { char c[900]; snprintf(c, sizeof c, "ln -s tree '%s/dirlink' && mkdir '%s/dst2'", root, root); if (system(c)) {} }
    CHECK(wmfo_copy(P("dirlink"), P("dst2"), out, sizeof out) == 0);                                                        /* symlink to dir is copied as a link, not followed */
    { struct stat st; CHECK(lstat(out, &st) == 0 && S_ISLNK(st.st_mode)); }

    /* move */
    CHECK(wmfo_move(P("b.txt"), P("New"), out, sizeof out) == 0 && has("New/b.txt") && !has("b.txt"));
    put("b.txt", "second"); CHECK(wmfo_move(P("b.txt"), P("New"), out, sizeof out) == 0 && has("New/b (copy).txt") && !strcmp(slurp("New/b (copy).txt"), "second"));
    CHECK(wmfo_move(P("New"), P("New"), NULL, 0) == -1 && has("New"));

    /* trash */
    put("gone.txt", "bye");
    CHECK(wmfo_trash(P("gone.txt")) == 0 && !has("gone.txt"));
    CHECK(has("home/.local/share/Trash/files/gone.txt"));
    { char *info = slurp("home/.local/share/Trash/info/gone.txt.trashinfo"); CHECK(info && strstr(info, "[Trash Info]") && strstr(info, "Path=") && strstr(info, "/gone.txt") && strstr(info, "DeletionDate=")); }
    put("gone.txt", "again"); CHECK(wmfo_trash(P("gone.txt")) == 0 && has("home/.local/share/Trash/files/gone.txt.1"));   /* name clash in trash */
    put("sp ace&x.txt", "x"); CHECK(wmfo_trash(P("sp ace&x.txt")) == 0);
    { char *info = slurp("home/.local/share/Trash/info/sp ace&x.txt.trashinfo"); CHECK(info && strstr(info, "sp%20ace%26x.txt")); }   /* percent-encoded */
    CHECK(wmfo_trash(P("tree")) == 0 && has("home/.local/share/Trash/files/tree/sub/f"));                                              /* whole folder */
    CHECK(wmfo_trash("/") == -1);  CHECK(wmfo_trash(home) == -1);  CHECK(wmfo_trash(P("home/.local/share")) == -1);                      /* guards */
    CHECK(wmfo_trash(P("does-not-exist")) == -1);
    { char c[900]; snprintf(c, sizeof c, "ln -s /nonexistent-target '%s/dangling'", root); if (system(c)) {} }
    CHECK(wmfo_trash(P("dangling")) == 0 && !has("dangling"));                                                                        /* dangling link */
    { char o[1024];
      CHECK(wmfo_restore(P("home/.local/share/Trash/files/dangling"), o, sizeof o) == 0 && has("dangling"));
      struct stat st; CHECK(lstat(P("dangling"), &st) == 0 && S_ISLNK(st.st_mode));
    }
    { char t[1024], o[1024];
      CHECK(symlink("/missing-relative-target", P("dangling-relative")) == 0);
      CHECK(chdir(root) == 0);
      CHECK(wmfo_trash_to("dangling-relative", t, sizeof t) == 0);
      CHECK(wmfo_restore(t, o, sizeof o) == 0 && !strcmp(o, P("dangling-relative")));
      struct stat st; CHECK(lstat(P("dangling-relative"), &st) == 0 && S_ISLNK(st.st_mode));
      CHECK(chdir("/") == 0);
    }
    setenv("XDG_DATA_HOME", P("xdg"), 1); put("x2.txt", "x"); CHECK(wmfo_trash(P("x2.txt")) == 0 && has("xdg/Trash/files/x2.txt"));       /* honours XDG_DATA_HOME */

    /* undo support: trash remembers where it went, restore puts it back (and picks a free name on a clash) */
    { char t[1024], o[1024];
      put("r me&.txt", "hi"); CHECK(wmfo_trash_to(P("r me&.txt"), t, sizeof t) == 0 && !has("r me&.txt"));
      CHECK(wmfo_restore(t, o, sizeof o) == 0 && has("r me&.txt") && !strcmp(o, P("r me&.txt")));
      CHECK(wmfo_restore(t, o, sizeof o) == -1);                                                    /* already restored */
      CHECK(wmfo_trash_to(P("r me&.txt"), t, sizeof t) == 0); put("r me&.txt", "new");
      CHECK(wmfo_restore(t, o, sizeof o) == 0 && has("r me&.txt") && has("r me&.txt") && strcmp(o, P("r me&.txt")) != 0);   /* original name taken */
      sh("mkdir '%s/gone'", root); sh("echo x > '%s/gone/f'", root);
      CHECK(wmfo_trash_to(P("gone/f"), t, sizeof t) == 0); sh("rm -rf '%s/gone'", root);
      CHECK(wmfo_restore(t, o, sizeof o) == -1 && strstr(wmfo_error(), "gone"));                    /* origin folder missing: refuses, keeps the file */
      CHECK(wmfo_restore("/etc/hostname", o, sizeof o) == -1); }

    sh("rm -rf '%s'", root);
    if (failures) { fprintf(stderr, "%d of %d file-op checks FAILED\n", failures, checks); return 1; }
    printf("all %d file-op unit checks passed\n", checks);
    return 0;
}
