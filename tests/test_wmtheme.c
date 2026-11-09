#define _POSIX_C_SOURCE 200809L
/* Unit tests for WMTheme: style-file parsing, textures, layering of global/user files, defaults,
 * colour parsing, bevel shades, font pattern mapping, defaults file location. */
#include "../core/theme/wmtheme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define STR(a, b) do { checks++; if (strcmp((a), (b))) { failures++; fprintf(stderr, "FAIL %s:%d: %s = \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a, (a), (b)); } } while (0)

static char dir[256];
static const char *wr(const char *name, const char *text) {
    static char p[512]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); if (!f) { perror(p); exit(2); } fputs(text, f); fclose(f); return p;
}

int main(void) {
    snprintf(dir, sizeof dir, "/tmp/wmtheme-test-XXXXXX"); if (!mkdtemp(dir)) return 2;
    WMTheme t;

    wmtheme_defaults(&t);
    STR(t.menu_text_back, "rgb:aa/aa/aa"); STR(t.menu_title_color, "white");

    /* every texture shape Window Maker writes */
    const char *u = wr("user",
        "/* header comment */\n{\n  // line comment\n"
        "  MenuTextBack = (dgradient, \"rgb:a6/a6/b6\", \"rgb:51/55/61\");\n"
        "  MenuTitleBack = (solid, black);\n"
        "  MenuTextColor = \"#102030\"; MenuTitleColor = white;\n"
        "  HighlightColor = gray90; HighlightTextColor = \"rgb:1/2/3\";\n"
        "  MenuDisabledColor = \"gray40\"; FrameBorderColor = \"#000\";\n"
        "  MenuTextFont = \"Noto Sans:pixelsize=14\"; MenuTitleFont = \"Noto Sans:bold:pixelsize=14\";\n"
        "  UnrelatedKey = (1, (nested, \"x\"), { a = b; });\n"
        "}\n");
    CHECK(wmtheme_load(&t, u, "/nonexistent") == 1);
    STR(t.menu_text_back, "rgb:a6/a6/b6"); STR(t.menu_title_back, "black");
    STR(t.menu_text_color, "#102030"); STR(t.highlight_color, "gray90"); STR(t.highlight_text_color, "rgb:1/2/3");
    STR(t.menu_disabled_color, "gray40"); STR(t.menu_text_font, "Noto Sans:pixelsize=14");
    STR(t.menu_title_font, "Noto Sans:bold:pixelsize=14");

    /* bare colour as texture, pixmap texture with fallback colour, pixmap without colour keeps the default */
    u = wr("u2", "{ MenuTextBack = \"#445566\"; MenuTitleBack = (tpixmap, \"file.xpm\", \"#112233\"); }");
    wmtheme_load(&t, u, "/nonexistent"); STR(t.menu_text_back, "#445566"); STR(t.menu_title_back, "#112233");
    u = wr("u3", "{ MenuTitleBack = (cpixmap, \"file.xpm\"); MenuTextBack = (mhgradient, \"#010203\", \"#040506\", \"#070809\"); }");
    wmtheme_load(&t, u, "/nonexistent"); STR(t.menu_title_back, "black"); STR(t.menu_text_back, "#010203");

    /* global file first, user file overrides only the keys it has */
    char g[512]; snprintf(g, sizeof g, "%s", wr("g", "{ MenuTextColor = red; MenuTitleColor = blue; MenuTextBack = (solid, \"#777777\"); }"));
    u = wr("u4", "{ MenuTextColor = green; }");
    CHECK(wmtheme_load(&t, u, g) == 2);
    STR(t.menu_text_color, "green"); STR(t.menu_title_color, "blue"); STR(t.menu_text_back, "#777777");

    /* broken or missing files fall back to the built-in look and never crash */
    const char *bad[] = { "{ MenuTextColor = ", "(not a dict)", "", "{ a = ( b, c ; }", "{ MenuTextColor = \"unterminated; }" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        u = wr("bad", bad[i]);
        CHECK(wmtheme_load(&t, u, "/nonexistent") == 0); STR(t.menu_text_color, "black");
    }
    CHECK(wmtheme_load(&t, "/nonexistent/a", "/nonexistent/b") == 0);
    { char deep[4096] = "{ k = "; for (int i = 0; i < 100; i++) strcat(deep, "("); strcat(deep, "x"); for (int i = 0; i < 100; i++) strcat(deep, ")"); strcat(deep, "; }");
      u = wr("deep", deep); CHECK(wmtheme_load(&t, u, "/nonexistent") == 0); }          /* nesting bomb is refused, not a stack overflow */

    /* colours */
    unsigned c;
    CHECK(wmtheme_color("#ff8000", &c) && c == 0xff8000);
    CHECK(wmtheme_color("#f80", &c) && c == 0xff8800);
    CHECK(wmtheme_color("#ffff80800000", &c) && c == 0xff8000);
    CHECK(wmtheme_color("rgb:a6/a6/b6", &c) && c == 0xa6a6b6);
    CHECK(wmtheme_color("rgb:f/0/8", &c) && c == 0xff0088);
    CHECK(wmtheme_color("RGB:ffff/0000/8080", &c) && c == 0xff0080);
    CHECK(wmtheme_color("black", &c) && c == 0 && wmtheme_color("White", &c) && c == 0xffffff);
    CHECK(wmtheme_color("gray50", &c) && c == 0x808080 && wmtheme_color("grey100", &c) && c == 0xffffff && wmtheme_color("gray0", &c) && c == 0);
    CHECK(!wmtheme_color("gray101", &c) && !wmtheme_color("#12", &c) && !wmtheme_color("rgb:1/2", &c) && !wmtheme_color("rgb:1/2/3/4", &c));
    CHECK(!wmtheme_color("#gg0000", &c) && !wmtheme_color("notacolour", &c) && !wmtheme_color(NULL, &c) && !wmtheme_color("rgb:12345/0/0", &c));

    /* shades keep ordering for light and dark faces */
    unsigned l, d; wmtheme_shades(0xaaaaaa, &l, &d); CHECK(l > 0xaaaaaa && d < 0xaaaaaa && (l & 255) > (d & 255));
    wmtheme_shades(0x000000, &l, &d); CHECK(l > 0 && d == 0);
    wmtheme_shades(0xffffff, &l, &d); CHECK(l == 0xffffff && d < 0xffffff);

    /* fonts */
    char pat[8][160]; int n;
    n = wmtheme_font_patterns("Sans:bold:pixelsize=12", pat, 8);
    CHECK(n >= 3); STR(pat[0], "-*-helvetica-bold-r-*-*-12-*-*-*-*-*-*-*"); CHECK(strstr(pat[n - 1], "-*-*-bold-r-*-*-12-") != NULL);
    n = wmtheme_font_patterns("Serif:italic:size=9", pat, 8);
    STR(pat[0], "-*-times-medium-i-*-*-12-*-*-*-*-*-*-*");                    /* 9 pt at 96 dpi = 12 px */
    n = wmtheme_font_patterns("-misc-fixed-bold-*-*-*-13-*-*-*-*-*-*-*,Sans", pat, 8);
    STR(pat[0], "-misc-fixed-bold-*-*-*-13-*-*-*-*-*-*-*"); CHECK(n > 1);     /* XLFD passes through, list continues */
    n = wmtheme_font_patterns("DejaVu Sans:weight=bold:slant=italic", pat, 8);
    STR(pat[0], "-*-dejavu sans-bold-i-*-*-12-*-*-*-*-*-*-*");                 /* default size 12 */
    CHECK(wmtheme_font_patterns("", pat, 8) == 0 && wmtheme_font_patterns(NULL, pat, 8) == 0);
    CHECK(wmtheme_font_patterns("Sans:bold,Serif,Mono", pat, 2) == 2);        /* respects max */

    /* where the user file lives, like wmaker's wusergnusteppath() */
    char p[512];
    setenv("HOME", "/h", 1); unsetenv("WMAKER_USER_ROOT");
    CHECK(wmtheme_defaults_path("WMWindowAttributes", p, sizeof p)); STR(p, "/h/GNUstep/Defaults/WMWindowAttributes");
    setenv("WMAKER_USER_ROOT", "/r", 1); wmtheme_user_path(p, sizeof p); STR(p, "/r/Defaults/WindowMaker");
    setenv("WMAKER_USER_ROOT", "~/gs", 1); wmtheme_user_path(p, sizeof p); STR(p, "/h/gs/Defaults/WindowMaker");
    unsetenv("HOME"); unsetenv("WMAKER_USER_ROOT"); CHECK(!wmtheme_user_path(p, sizeof p));

    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
