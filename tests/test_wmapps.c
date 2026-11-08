#define _POSIX_C_SOURCE 200809L
/* Unit tests for WMApps: Exec tokenizing, field codes, quoting, Window Maker escaping,
 * ID masking, locale selection, nested dirs, launching (no shell), and WMWatch.
 * Builds its own throw-away data dirs under a mkdtemp() directory. */
#include "../core/applications/wmapps.h"
#include "../core/applications/wmmenu.h"
#include "../core/applications/wmwatch.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(got, want) do { checks++; const char *g_ = (got), *w_ = (want); \
    if (!g_ || strcmp(g_, w_)) { failures++; fprintf(stderr, "FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, g_ ? g_ : "(null)", w_); } } while (0)

static char root[512];

static void put(const char *rel, const char *text) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", root, rel);
    for (char *s = p + strlen(root) + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    FILE *f = fopen(p, "w"); if (!f) { perror(p); exit(2); } fputs(text, f); fclose(f);
}
static void rm_rf(const char *p) { char c[1100]; snprintf(c, sizeof c, "rm -rf '%s'", p); if (system(c)) {} }

static const WMApp *by_id(const WMAppRegistry *r, const char *id) {
    for (size_t i = 0; i < r->count; i++) if (!strcmp(r->apps[i].id, id)) return &r->apps[i];
    return NULL;
}

static void test_tokenize(void) {
    char **v; size_t n;
    CHECK(!wmapps_tokenize("a b  c", &v, &n)); CHECK(n == 3); CHECK_STR(v[2], "c"); wmapps_argv_free(v);
    CHECK(!wmapps_tokenize("sh -c \"echo \\\"hi\\\" \\$HOME; ls\" x", &v, &n));
    CHECK(n == 4); CHECK_STR(v[2], "echo \"hi\" $HOME; ls"); CHECK_STR(v[3], "x"); wmapps_argv_free(v);
    CHECK(!wmapps_tokenize("prog \"a b\"\"c\" d", &v, &n)); CHECK(n == 3); CHECK_STR(v[1], "a bc"); wmapps_argv_free(v);
    CHECK(!wmapps_tokenize("prog ''", &v, &n)); CHECK(n == 2); CHECK_STR(v[1], ""); wmapps_argv_free(v);          /* empty arg survives */
    CHECK(!wmapps_tokenize("sh -c 'echo $x; y'", &v, &n)); CHECK_STR(v[2], "echo $x; y"); wmapps_argv_free(v);     /* tolerated single quotes */
    CHECK(!wmapps_tokenize("a\\ b c", &v, &n)); CHECK(n == 2); CHECK_STR(v[0], "a b"); wmapps_argv_free(v);
    CHECK(wmapps_tokenize("prog \"unterminated", &v, &n) == -1);
    CHECK(wmapps_tokenize("prog trailing\\", &v, &n) == -1);
    CHECK(!wmapps_tokenize("   ", &v, &n)); CHECK(n == 0); wmapps_argv_free(v);
}

static void test_quote(void) {
    char *q;
    q = wmapps_shell_quote("plain-1.2/x"); CHECK_STR(q, "plain-1.2/x"); free(q);
    q = wmapps_shell_quote(""); CHECK_STR(q, "''"); free(q);
    q = wmapps_shell_quote("it's $(rm -rf ~)"); CHECK_STR(q, "'it'\\''s $(rm -rf ~)'"); free(q);
    q = wmapps_shell_quote("a;b"); CHECK_STR(q, "'a;b'"); free(q);
    q = wmmenu_wm_escape("echo 100% \\n %w"); CHECK_STR(q, "echo 100\\% \\\\n \\%w"); free(q);
}

static void test_registry(void) {
    snprintf(root, sizeof root, "/tmp/wmapps-test-XXXXXX"); if (!mkdtemp(root)) { perror("mkdtemp"); exit(2); }
    char hi[600]; snprintf(hi, sizeof hi, "%s/hi", root);
    /* high priority dir masks low priority ones, even with Hidden/NoDisplay */
    put("hi/applications/masked.desktop", "[Desktop Entry]\nType=Application\nName=Masked\nExec=masked\nHidden=true\n");
    put("lo/applications/masked.desktop", "[Desktop Entry]\nType=Application\nName=Masked Low\nExec=masked-low\n");
    put("hi/applications/over.desktop", "[Desktop Entry]\nType=Application\nName=Over High\nExec=over-hi\n");
    put("lo/applications/over.desktop", "[Desktop Entry]\nType=Application\nName=Over Low\nExec=over-lo\n");
    put("lo/applications/only.desktop", "[Desktop Entry]\nType=Application\nName=Only Low\nExec=only\n");
    /* nested vendor dirs, three deep */
    put("lo/applications/a/b/c/deep.desktop", "[Desktop Entry]\nType=Application\nName=Deep\nExec=deep\n");
    /* locale */
    put("lo/applications/loc.desktop",
        "[Desktop Entry]\nType=Application\nName=Plain\nName[de]=Deutsch\nName[de_AT]=Oesterreichisch\nName[de@mod]=DeMod\n"
        "Name[de_AT@mod]=Alles\nName[fr]=Francais\nExec=loc\n");
    /* Exec / field codes */
    put("lo/applications/ex.desktop",
        "[Desktop Entry]\nType=Application\nName=Ex Ample\nIcon=exicon\nExec=sh -c \"echo \\\\\\\"100%%\\\\\\\"; \\\\$HOME\" --name=%c --file=%f %i %F\n");
    put("lo/applications/term.desktop", "[Desktop Entry]\nType=Application\nName=Term\nExec=htop\nTerminal=true\nPath=/tmp\n");
    put("lo/applications/spacey.desktop", "[Desktop Entry]\nType=Application\nName=Spacey\nExec=\"/opt/My App/bin/run\" %U\n");
    /* only/not show in, trailing garbage, broken */
    put("lo/applications/gnome.desktop", "[Desktop Entry]\nType=Application\nName=Gnome\nExec=g\nOnlyShowIn=GNOME;\n");
    put("lo/applications/notwm.desktop", "[Desktop Entry]\nType=Application\nName=NotWM\nExec=n\nNotShowIn=WindowMaker;\n");
    put("lo/applications/wmonly.desktop", "[Desktop Entry]\nType=Application\nName=WM Only\nExec=w\nOnlyShowIn=GNOME;WindowMaker;\n");
    put("lo/applications/badquote.desktop", "[Desktop Entry]\nType=Application\nName=Bad\nExec=prog \"oops\n");
    put("lo/applications/noname.desktop", "[Desktop Entry]\nType=Application\nExec=x\n");
    put("lo/applications/link.desktop", "[Desktop Entry]\nType=Link\nName=Link\nURL=http://x\n");
    put("lo/applications/flat.desktop", "[Desktop Entry]\nType=Application\nName=Flat\nExec=/usr/bin/flatpak run --command=flat org.example.Flat @@u %U @@\nX-Flatpak=org.example.Flat\n");
    put("lo/applications/second.desktop", "[Desktop Entry]\nType=Application\nName=Second\nExec=s\n[Desktop Entry]\nName=Ignored\n[Other]\nName=AlsoIgnored\n");
    /* a long line (>4096) must not truncate or split */
    { char big[9000]; strcpy(big, "[Desktop Entry]\nType=Application\nName=Long\nComment="); size_t l = strlen(big);
      memset(big + l, 'x', 5000); big[l + 5000] = 0; strcat(big, "\nExec=long\n"); put("lo/applications/long.desktop", big); }

    char lo[600]; snprintf(lo, sizeof lo, "%s/lo", root);
    setenv("HOME", "/nonexistent", 1); setenv("XDG_DATA_HOME", hi, 1); setenv("XDG_DATA_DIRS", lo, 1);
    unsetenv("XDG_CURRENT_DESKTOP"); setenv("LANG", "C", 1); unsetenv("LC_ALL"); unsetenv("LC_MESSAGES");

    WMAppRegistry r; wmapps_scan(&r, NULL, NULL);
    CHECK(!by_id(&r, "masked"));                         /* Hidden=true in a higher dir deletes the lower entry */
    CHECK(by_id(&r, "over") && !strcmp(by_id(&r, "over")->name, "Over High"));
    CHECK(by_id(&r, "only"));
    CHECK(by_id(&r, "a-b-c-deep"));                      /* nested id = path with '/' -> '-' */
    CHECK(!by_id(&r, "gnome")); CHECK(!by_id(&r, "notwm")); CHECK(by_id(&r, "wmonly"));
    CHECK(!by_id(&r, "badquote")); CHECK(!by_id(&r, "noname")); CHECK(!by_id(&r, "link"));
    int bq = 0, nn = 0, lk = 0;
    for (size_t i = 0; i < r.nskipped; i++) { bq += !!strstr(r.skipped[i], "badquote.desktop: malformed Exec"); nn += !!strstr(r.skipped[i], "noname.desktop: missing Name"); lk += !!strstr(r.skipped[i], "link.desktop"); }
    CHECK(bq == 1); CHECK(nn == 1); CHECK(lk == 0);      /* broken ones are reported; Type=Link is just not an app */
    const WMApp *s = by_id(&r, "second"); CHECK(s && !strcmp(s->name, "Second"));
    const WMApp *lg = by_id(&r, "long"); CHECK(lg && strlen(lg->comment) == 5000);
    const WMApp *fl = by_id(&r, "flat"); CHECK(fl && fl->origin == WMAPP_FLATPAK && !strcmp(fl->flatpak_id, "org.example.Flat"));
    CHECK(fl && wmapps_accepts_files(fl) == 2);
    wmapps_free(&r);

    /* locale precedence */
    const char *cases[][2] = { {"C", "Plain"}, {"de_DE.UTF-8", "Deutsch"}, {"de_AT.UTF-8", "Oesterreichisch"},
                               {"de_DE@mod", "DeMod"}, {"de_AT.UTF-8@mod", "Alles"}, {"fr_CA", "Francais"}, {"ja_JP", "Plain"} };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        setenv("LANG", cases[i][0], 1); wmapps_scan(&r, NULL, NULL);
        const WMApp *a = by_id(&r, "loc"); CHECK(a); if (a) CHECK_STR(a->name, cases[i][1]);
        wmapps_free(&r);
    }
    setenv("LANG", "C", 1);

    /* field codes & shell-safe command */
    wmapps_scan(&r, NULL, NULL);
    const WMApp *ex = by_id(&r, "ex");
    CHECK(ex && ex->argc == 7);
    size_t n; char **v = wmapps_expand(ex, NULL, 0, &n);       /* no files: %f -> "--file=", %i -> --icon exicon, %F -> nothing */
    CHECK(n == 7 && !strcmp(v[2], "echo \"100%\"; $HOME"));
    CHECK(!strcmp(v[3], "--name=Ex Ample") && !strcmp(v[4], "--file=") && !strcmp(v[5], "--icon") && !strcmp(v[6], "exicon"));
    wmapps_argv_free(v);
    const char *files[] = { "/tmp/a b.txt", "file:///tmp/c%20d.txt", "relative.txt" };
    v = wmapps_expand(ex, files, 3, &n);
    CHECK(n == 7 + 3 &&!strcmp(v[4], "--file=/tmp/a b.txt") && !strcmp(v[5], "--icon") && !strcmp(v[6], "exicon"));
    CHECK(!strcmp(v[7], "/tmp/a b.txt") && !strcmp(v[8], "/tmp/c d.txt") && !strcmp(v[9], "relative.txt"));
    wmapps_argv_free(v);
    char *cmd = wmapps_shell_command(ex, NULL, 0, NULL);
    CHECK_STR(cmd, "sh -c 'echo \"100%\"; $HOME' '--name=Ex Ample' --file= --icon exicon"); free(cmd);

    const WMApp *sp = by_id(&r, "spacey");
    const char *u[] = { "/tmp/x y.txt", "https://example.org/a b" };
    v = wmapps_expand(sp, u, 2, &n);
    CHECK(n == 3 && !strcmp(v[0], "/opt/My App/bin/run") && !strcmp(v[1], "file:///tmp/x%20y.txt") && !strcmp(v[2], "https://example.org/a b"));
    wmapps_argv_free(v);
    v = wmapps_expand(by_id(&r, "ex"), (const char *[]){ "https://example.org/x" }, 1, &n);  /* %f cannot take a URL: dropped */
    CHECK(!strcmp(v[4], "--file="));
    wmapps_argv_free(v);

    cmd = wmapps_shell_command(by_id(&r, "term"), NULL, 0, "kitty --hold -e");
    CHECK_STR(cmd, "kitty --hold -e htop"); free(cmd);
    cmd = wmapps_shell_command(by_id(&r, "term"), NULL, 0, NULL); CHECK_STR(cmd, "htop"); free(cmd);

    /* window matching (Dock): class, instance, exec basename, id tail; flatpak "run" must not match */
    put("lo/applications/org.gnome.Calculator.desktop", "[Desktop Entry]\nType=Application\nName=Calc\nExec=gnome-calculator %U\nStartupWMClass=gnome-calculator\n");
    put("lo/applications/vendor-bin.desktop", "[Desktop Entry]\nType=Application\nName=Binary\nExec=/usr/lib/thing/realbin --x\n");
    wmapps_free(&r); wmapps_scan(&r, NULL, NULL);
    const WMApp *mw;
    mw = wmapps_match_window(&r, "gnome-calculator", "Gnome-calculator"); CHECK(mw && !strcmp(mw->id, "org.gnome.Calculator"));
    mw = wmapps_match_window(&r, "whatever", "Calculator");               CHECK(mw && !strcmp(mw->id, "org.gnome.Calculator"));   /* id tail */
    mw = wmapps_match_window(&r, "realbin", "Foo");                       CHECK(mw && !strcmp(mw->id, "vendor-bin"));            /* exec basename */
    mw = wmapps_match_window(&r, "flatpak", "Flatpak");                   CHECK(!mw);
    mw = wmapps_match_window(&r, "zzz", "Zzz");                           CHECK(!mw);
    mw = wmapps_match_window(&r, NULL, NULL);                             CHECK(!mw);

    /* menu output: no injection, correct layer escaping per backend */
    put("lo/applications/evil.desktop", "[Desktop Entry]\nType=Application\nName=Evil \"Q\"\nExec=sh -c \"echo %%w; rm -rf $HOME\\\\\\\\\"\nCategories=Game;\n");
    wmapps_free(&r); wmapps_scan(&r, NULL, NULL);
    char *mb = NULL; size_t ml = 0; FILE *m = open_memstream(&mb, &ml);
    wmmenu_emit_applications(m, &r, "xterm -e"); fclose(m);
    CHECK(strstr(mb, "(\"Evil \\\"Q\\\"\", SHEXEC, \"sh -c 'echo \\\\%w; rm -rf $HOME\\\\\\\\'\")") != NULL);
    CHECK(strstr(mb, "SHEXEC, \"xterm -e htop\")") != NULL);
    free(mb);
    wmapps_free(&r);
}

static void test_launch(void) {
    char out[700]; snprintf(out, sizeof out, "%s/launched", root);
    char body[1500];
    /* Exec has shell metacharacters in an argument: they must arrive as data, not run. */
    snprintf(body, sizeof body,
             "[Desktop Entry]\nType=Application\nName=Rec\nExec=sh -c \"printf '%%%%s|' \\\\\"$@\\\\\" >> '%s'\" rec %%F\n", out);
    put("lo/applications/rec.desktop", body);
    snprintf(body, sizeof body, "[Desktop Entry]\nType=Application\nName=One\nExec=sh -c \"printf '%%%%s|' \\\\\"$1\\\\\" >> '%s'\" one %%f\n", out);
    put("lo/applications/one.desktop", body);
    put("lo/applications/nope.desktop", "[Desktop Entry]\nType=Application\nName=Nope\nExec=/definitely/not/here --x\n");
    put("lo/applications/nofiles.desktop", "[Desktop Entry]\nType=Application\nName=NoFiles\nExec=true\n");
    WMAppRegistry r; wmapps_scan(&r, NULL, NULL);
    char err[256];
    const char *hostile[] = { "a;touch /tmp/PWNED", "$(touch /tmp/PWNED)", "b c" };
    CHECK(!wmapps_launch(by_id(&r, "rec"), hostile, 3, NULL, err, sizeof err));
    CHECK(!wmapps_launch(by_id(&r, "one"), hostile, 3, NULL, err, sizeof err));       /* one instance per file */
    for (int i = 0; i < 40 && access(out, F_OK); i++) usleep(50000);
    usleep(300000);
    FILE *f = fopen(out, "r"); char got[512] = ""; if (f) { size_t k = fread(got, 1, sizeof got - 1, f); got[k] = 0; fclose(f); }
    CHECK(strstr(got, "a;touch /tmp/PWNED|$(touch /tmp/PWNED)|b c|") != NULL);
    CHECK(access("/tmp/PWNED", F_OK) != 0);
    int n3 = 0; for (const char *p = got; (p = strstr(p, "$(touch /tmp/PWNED)|")); p++) n3++; CHECK(n3 == 2);
    CHECK(wmapps_launch(by_id(&r, "nope"), NULL, 0, NULL, err, sizeof err) == -1 && strstr(err, "No such file"));   /* exec failure is reported */
    CHECK(wmapps_launch(by_id(&r, "nofiles"), hostile, 1, NULL, err, sizeof err) == -1 && strstr(err, "does not accept"));
    CHECK(!wmapps_launch(by_id(&r, "nofiles"), NULL, 0, NULL, err, sizeof err));
    wmapps_free(&r);
}

static int wait_event(WMWatch *w, int ms) {
    struct pollfd p = { wmwatch_fd(w), POLLIN, 0 };
    int changed = 0;
    for (int waited = 0; waited < ms && !changed; waited += 50)
        if (poll(&p, 1, 50) > 0) changed = wmwatch_poll(w);
    return changed;
}

static void test_watch(void) {
    char hi[600], lo[600]; snprintf(hi, sizeof hi, "%s/hi", root); snprintf(lo, sizeof lo, "%s/lo", root);
    setenv("XDG_DATA_HOME", hi, 1); setenv("XDG_DATA_DIRS", lo, 1);
    WMWatch *w = wmwatch_open(NULL); CHECK(w);
    if (!w) return;
    CHECK(wmwatch_count(w) == 5);                         /* hi/applications, lo/applications, a, a/b, a/b/c */
    put("lo/applications/a/b/c/new.desktop", "[Desktop Entry]\nType=Application\nName=New\nExec=n\n");
    CHECK(wait_event(w, 1500));                           /* three levels down */
    put("lo/applications/a/b/c/readme.txt", "x");
    CHECK(!wait_event(w, 400));                           /* non-.desktop files are ignored */
    put("lo/applications/fresh/deeper/x.desktop", "[Desktop Entry]\nType=Application\nName=Fresh\nExec=n\n");
    wait_event(w, 1500);
    put("lo/applications/fresh/deeper/y.desktop", "[Desktop Entry]\nType=Application\nName=Fresh2\nExec=n\n");
    CHECK(wait_event(w, 1500));                           /* a directory created after startup is watched */
    char dd[700]; snprintf(dd, sizeof dd, "%s/lo/applications/fresh", root); rm_rf(dd);
    CHECK(wait_event(w, 1500));
    wmwatch_close(w);

    /* applications/ does not exist yet: parent is watched until it appears */
    char nd[700]; snprintf(nd, sizeof nd, "%s/late", root); mkdir(nd, 0755);
    setenv("XDG_DATA_DIRS", nd, 1);
    w = wmwatch_open(NULL); CHECK(w);
    put("late/applications/z.desktop", "[Desktop Entry]\nType=Application\nName=Z\nExec=z\n");
    CHECK(wait_event(w, 1500));
    put("late/applications/z2.desktop", "[Desktop Entry]\nType=Application\nName=Z2\nExec=z\n");
    CHECK(wait_event(w, 1500));
    wmwatch_close(w);
}

int main(void) {
    test_tokenize(); test_quote(); test_registry(); test_launch(); test_watch();
    rm_rf(root);
    if (failures) { fprintf(stderr, "%d of %d checks FAILED\n", failures, checks); return 1; }
    printf("all %d WMApps unit checks passed\n", checks);
    return 0;
}
