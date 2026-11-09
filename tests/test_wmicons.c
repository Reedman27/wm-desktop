#define _POSIX_C_SOURCE 200809L
/* Unit tests for WMIcons: theme inheritance, index.theme distances, symbolic skipping, SVG
 * rendering + cache, converter-less fallback, name guessing, generated XPM. Isolated under mkdtemp(). */
#include "../core/applications/wmicons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char root[512];
static void put(const char *rel, const char *text) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", root, rel);
    for (char *s = p + strlen(root) + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    FILE *f = fopen(p, "w"); if (!f) { perror(p); exit(2); } fputs(text, f); fclose(f);
}
static int ends(const char *s, const char *suf) { size_t n = strlen(s), m = strlen(suf); return n >= m && !strcmp(s + n - m, suf); }
static int has(const char *s, const char *sub) { return strstr(s, sub) != NULL; }

static WMApp mkapp(const char *id, const char *name, const char *icon, const char *cls) {
    WMApp a; memset(&a, 0, sizeof a);
    a.id = (char *)id; a.name = (char *)name; a.icon = (char *)icon; a.wm_class = (char *)cls;
    a.generic_name = a.comment = a.exec = a.category = a.mime_types = a.work_dir = a.flatpak_id = a.path = (char *)"";
    return a;
}

static void themes(void) {
    char data[600]; snprintf(data, sizeof data, "%s/data", root);
    setenv("HOME", root, 1); setenv("XDG_DATA_HOME", data, 1); setenv("XDG_DATA_DIRS", "/nonexistent", 1);
    unsetenv("XDG_CONFIG_HOME"); setenv("WMCONFIG_FILE", "/nonexistent/settings.conf", 1); unsetenv("WMICONS_THEME");

    put("data/icons/Fancy/index.theme", "[Icon Theme]\nName=Fancy\nInherits=Base\nDirectories=64x64/apps,symbolic/apps\n"
        "[64x64/apps]\nSize=64\nContext=Applications\nType=Fixed\n[symbolic/apps]\nSize=64\nContext=Applications\nType=Scalable\nMinSize=8\nMaxSize=512\n");
    put("data/icons/Fancy/64x64/apps/bar.png", "x");
    put("data/icons/Fancy/symbolic/apps/onlysym.svg", "<svg/>");       /* monochrome dirs must never be used */
    put("data/icons/Base/index.theme", "[Icon Theme]\nName=Base\nDirectories=48x48/apps,16x16/apps,256x256/apps,32x32/mimetypes\n"
        "[48x48/apps]\nSize=48\nContext=Applications\nType=Fixed\n[16x16/apps]\nSize=16\nContext=Applications\nType=Fixed\n"
        "[256x256/apps]\nSize=256\nContext=Applications\nType=Threshold\nThreshold=8\n[32x32/mimetypes]\nSize=32\nContext=MimeTypes\nType=Fixed\n");
    put("data/icons/Base/48x48/apps/foo.png", "x");
    put("data/icons/Base/48x48/apps/bar.png", "x");                    /* shadowed by Fancy */
    put("data/icons/Base/16x16/apps/small.png", "x");
    put("data/icons/Base/256x256/apps/huge.png", "x");
    put("data/icons/Base/32x32/mimetypes/mime.png", "x");              /* wrong context */
    put("data/icons/hicolor/48x48/apps/hic.png", "x");                 /* no index.theme: dir names are inferred */
    put("data/icons/Other/32x32/apps/odd.png", "x");

    char p[4096]; int r;
    setenv("WMICONS_THEME", "Fancy", 1);
    r = wmicons_resolve("bar", 64, p, sizeof p);   CHECK(r == 1 && has(p, "/Fancy/64x64/apps/bar.png"));     /* theme beats its parent */
    r = wmicons_resolve("foo", 64, p, sizeof p);   CHECK(r == 1 && has(p, "/Base/48x48/apps/foo.png"));      /* Inherits chain */
    r = wmicons_resolve("hic", 64, p, sizeof p);   CHECK(r == 1 && has(p, "/hicolor/48x48/apps/hic.png"));   /* hicolor fallback */
    r = wmicons_resolve("odd", 64, p, sizeof p);   CHECK(r == 1 && has(p, "/Other/"));                       /* any other theme last */
    r = wmicons_resolve("onlysym", 64, p, sizeof p); CHECK(r == 0);
    r = wmicons_resolve("mime", 64, p, sizeof p);  CHECK(r == 0);
    r = wmicons_resolve("huge", 256, p, sizeof p); CHECK(r == 1);       /* Threshold 8 around 256 */
    r = wmicons_resolve("small.png", 64, p, sizeof p); CHECK(r == 1 && has(p, "small.png"));                 /* stray extension */
    r = wmicons_resolve("nothing-here", 64, p, sizeof p); CHECK(r == 0);
    put("data/icons/Sized/index.theme", "[Icon Theme]\nName=Sized\nDirectories=48x48/apps,128x128/apps\n"
        "[48x48/apps]\nSize=48\nContext=Applications\nType=Fixed\n[128x128/apps]\nSize=128\nContext=Applications\nType=Fixed\n");
    put("data/icons/Sized/48x48/apps/both.png", "x"); put("data/icons/Sized/128x128/apps/both.png", "x");
    setenv("WMICONS_THEME", "Sized", 1);
    r = wmicons_resolve("both", 64, p, sizeof p);  CHECK(r == 1 && has(p, "/128x128/"));     /* downscale beats upscale */
    r = wmicons_resolve("both", 48, p, sizeof p);  CHECK(r == 1 && has(p, "/48x48/"));       /* exact size wins when it exists */
    r = wmicons_resolve("both", 256, p, sizeof p); CHECK(r == 1 && has(p, "/128x128/"));     /* nothing bigger: take the biggest */
    setenv("WMICONS_THEME", "Fancy", 1);
    put("abs.png", "x"); char ap[700]; snprintf(ap, sizeof ap, "%s/abs.png", root);
    r = wmicons_resolve(ap, 64, p, sizeof p);      CHECK(r == 1 && !strcmp(p, ap));

    /* user theme from GTK settings, used when nothing is forced */
    unsetenv("WMICONS_THEME");
    put("data/icons/Gtkish/index.theme", "[Icon Theme]\nName=Gtkish\nDirectories=64x64/apps\n[64x64/apps]\nSize=64\nContext=Applications\nType=Fixed\n");
    put("data/icons/Gtkish/64x64/apps/foo.png", "x");
    put(".config/gtk-3.0/settings.ini", "[Settings]\ngtk-icon-theme-name = Gtkish\n");
    r = wmicons_resolve("foo", 64, p, sizeof p);   CHECK(r == 1 && has(p, "/Gtkish/"));
    /* the wm-desktop setting wins over GTK's */
    put("settings.conf", "icon_theme = Base\n");
    char sc[700]; snprintf(sc, sizeof sc, "%s/settings.conf", root); setenv("WMCONFIG_FILE", sc, 1);
    r = wmicons_resolve("foo", 64, p, sizeof p);   CHECK(r == 1 && has(p, "/Base/"));
}

static void svg_and_fallback(void) {
    char cache[600]; snprintf(cache, sizeof cache, "%s/cache", root); setenv("XDG_CACHE_HOME", cache, 1);
    put("data/icons/hicolor/scalable/apps/vec.svg",
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"128\" height=\"128\" viewBox=\"0 0 128 128\"><rect width=\"128\" height=\"128\" rx=\"20\" fill=\"#36c\"/></svg>");
    char sp[700]; snprintf(sp, sizeof sp, "%s/data/icons/hicolor/scalable/apps/vec.svg", root);
    char p[4096], q[4096];
    CHECK(wmicons_resolve("vec", 64, p, sizeof p) == 2 && !strcmp(p, sp));

    int have_conv = wmicons_convert_svg(sp, 64, p, sizeof p);
    if (have_conv) {
        FILE *f = fopen(p, "rb"); unsigned char m[8] = {0}; if (f) { size_t k = fread(m, 1, 8, f); (void)k; fclose(f); }
        CHECK(ends(p, ".png") && !memcmp(m, "\x89PNG\r\n\x1a\n", 8));
        struct stat s1, s2; stat(p, &s1);
        CHECK(wmicons_convert_svg(sp, 64, q, sizeof q) && !strcmp(p, q));                  /* cache hit, same file */
        stat(q, &s2); CHECK(s1.st_mtime == s2.st_mtime);
        put("data/icons/hicolor/scalable/apps/vec.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\"><circle cx=\"32\" cy=\"32\" r=\"30\"/></svg>");
        CHECK(wmicons_convert_svg(sp, 64, q, sizeof q) && strcmp(p, q));                   /* changed SVG -> new render */
        WMApp a = mkapp("vec", "Vec", "vec", ""); int g = -1;
        CHECK(wmicons_for_app(&a, 64, p, sizeof p, &g) == WMICON_CONVERTED && g == 0);
    } else fprintf(stderr, "note: no SVG converter installed, converted-icon checks skipped\n");

    /* converter unavailable: must fall back to a generated tile, never to nothing */
    char *oldpath = strdup(getenv("PATH")); setenv("PATH", "/nonexistent", 1);
    CHECK(wmicons_convert_svg(sp, 48, p, sizeof p) == 0 || have_conv);                     /* (cache hit possible for 64 only) */
    WMApp b = mkapp("vec2", "Vector Two", "vec", ""); int g = -1;
    WMIconSource s = wmicons_for_app(&b, 33, p, sizeof p, &g);
    CHECK(s == WMICON_GENERATED && ends(p, ".xpm"));
    setenv("PATH", oldpath, 1); free(oldpath);

    /* invalid SVG (converter rejects it) also ends in a tile */
    put("data/icons/hicolor/scalable/apps/broken.svg", "<svg/>");
    WMApp c = mkapp("broken", "Broken", "broken", ""); s = wmicons_for_app(&c, 64, p, sizeof p, &g);
    CHECK(s == WMICON_GENERATED);

}

static void guessing_and_xpm(void) {
    char p[4096]; int g = -1;
    /* id tail "Hic" -> "hic" resolves, so it must be a FILE hit found by guessing */
    WMApp d = mkapp("org.example.Hic", "Hic", "does-not-exist", "");
    WMIconSource s = wmicons_for_app(&d, 64, p, sizeof p, &g);
    CHECK(s == WMICON_FILE && g == 1 && has(p, "hic.png"));
    WMApp e = mkapp("whatever", "Whatever", "", "FOO"); s = wmicons_for_app(&e, 64, p, sizeof p, &g);
    CHECK(s == WMICON_FILE && g == 1 && has(p, "foo.png"));                               /* StartupWMClass lower-cased */
    WMApp f = mkapp("direct", "Direct", "small", ""); s = wmicons_for_app(&f, 16, p, sizeof p, &g);
    CHECK(s == WMICON_FILE && g == 0);                                                    /* Icon= used as given: not guessed */

    /* generated tile: valid XPM, deterministic, colour follows the ID, initial follows the name */
    char a1[4096], a2[4096], a3[4096], a4[4096];
    CHECK(wmicons_generate("app.one", "Alpha", 64, a1, sizeof a1) && wmicons_generate("app.one", "Alpha", 64, a2, sizeof a2) && !strcmp(a1, a2));
    CHECK(wmicons_generate("app.two", "Alpha", 64, a3, sizeof a3) && strcmp(a1, a3));
    CHECK(wmicons_generate("app.one", "9lives", 64, a4, sizeof a4) && strcmp(a1, a4));
    FILE *fp = fopen(a1, "r"); char l1[64] = "", l2[64] = "", l3[64] = "", l4[64] = "";
    if (fp) { if (!fgets(l1, sizeof l1, fp) || !fgets(l2, sizeof l2, fp) || !fgets(l3, sizeof l3, fp) || !fgets(l4, sizeof l4, fp)) {} fclose(fp); }
    CHECK(!strcmp(l1, "/* XPM */\n") && has(l3, "\"64 64 5 1\"") && has(l4, "None"));
    fp = fopen(a1, "r"); int rows = 0, bad = 0; char line[256];
    if (fp) { while (fgets(line, sizeof line, fp)) if (line[0] == '"' && line[1] != '6' && line[1] != 't' && line[1] != '.' && line[1] != 'o' && line[1] != 'h' && line[1] != 'x') bad++; else if (line[0] == '"') rows++; fclose(fp); }
    CHECK(bad == 0 && rows >= 64 + 5);                                                    /* header + 5 colours + 64 pixel rows */
    char t1[4096]; CHECK(wmicons_generate("tiny", "", 8, t1, sizeof t1) && has(t1, "-32-"));  /* size clamped to 32 */
    char t2[4096]; CHECK(wmicons_generate("huge", "Zed", 999, t2, sizeof t2) && has(t2, "-128-"));
    CHECK(!strcmp(wmicons_source_name(WMICON_GENERATED), "generated"));
}

int main(void) {
    snprintf(root, sizeof root, "/tmp/wmicons-test-XXXXXX"); if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    themes(); svg_and_fallback(); guessing_and_xpm();
    char c[600]; snprintf(c, sizeof c, "rm -rf '%s'", root); if (system(c)) {}
    if (failures) { fprintf(stderr, "%d of %d icon checks FAILED\n", failures, checks); return 1; }
    printf("all %d WMIcons unit checks passed\n", checks);
    return 0;
}
