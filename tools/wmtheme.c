#define _POSIX_C_SOURCE 200809L
/* wmtheme - show how the add-ons see your Window Maker theme.
 *
 *   wmtheme show      the colours, textures and fonts wmnotify and wmfiles will use, and which files they came from
 *   wmtheme path      the Window Maker defaults file that is watched for theme changes
 *
 * Use it first when something looks wrong: it prints exactly what core/theme/wmtheme.c resolved
 * (your defaults file, then the global one, then Window Maker's built-in values). */
#include "../core/theme/wmtheme.h"
#include <stdio.h>
#include <string.h>

static const char *kind_name[] = { "solid", "hgradient", "vgradient", "dgradient", "mhgradient", "mvgradient", "mdgradient" };

static void tex(const char *name, const WMTexture *t) {
    printf("%-20s %s", name, kind_name[t->kind]);
    for (int i = 0; i < t->n; i++) printf(" %s", t->color[i]);
    putchar('\n');
}

int main(int argc, char **argv) {
    char user[4096] = "";
    if (argc < 2) { fputs("usage: wmtheme show | path\n", stderr); return 2; }
    int have = wmtheme_user_path(user, sizeof user);
    if (!strcmp(argv[1], "path")) { if (!have) { fputs("wmtheme: no HOME\n", stderr); return 1; } puts(user); return 0; }
    if (strcmp(argv[1], "show")) { fprintf(stderr, "wmtheme: unknown command %s\n", argv[1]); return 2; }
    WMTheme t; int n = wmtheme_load(&t, NULL, NULL);
    printf("user file: %s%s\n", have ? user : "(no HOME)", have && wmtheme_mtime() ? "" : "  (not present)");
    printf("files read: %d%s\n", n, n > 0 ? "" : "  (Window Maker's built-in defaults)");
    tex("MenuTitleBack", &t.menu_title_tex); tex("MenuTextBack", &t.menu_text_tex);
    printf("%-20s %s\n%-20s %s\n%-20s %s\n", "MenuTitleColor", t.menu_title_color, "MenuTextColor", t.menu_text_color, "MenuDisabledColor", t.menu_disabled_color);
    printf("%-20s %s\n%-20s %s\n%-20s %s\n", "HighlightColor", t.highlight_color, "HighlightTextColor", t.highlight_text_color, "FrameBorderColor", t.frame_border_color);
    printf("%-20s %s\n%-20s %s\n", "MenuTitleFont", t.menu_title_font, "MenuTextFont", t.menu_text_font);
    return 0;
}
