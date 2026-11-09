#define _POSIX_C_SOURCE 200809L
/* Test helper: inspect and poke top-level X windows without xdotool.
 *   xprobe find CLASS            one line per mapped-or-not root child with that WM_CLASS class: "id mapped x y w h"
 *   xprobe pixel ID X Y          colour of one pixel as rrggbb
 *   xprobe count ID RRGGBB       how many pixels of that colour the window has
 *   xprobe click ID X Y          synthetic left click (press + release) at window coordinates
 *   xprobe key ID KEYSYM         synthetic key press + release, e.g. Down, q */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    Display *d = XOpenDisplay(NULL); if (!d) return 1;
    int rc = 0;
    if (!strcmp(argv[1], "find")) {
        Window root = DefaultRootWindow(d), r, p, *kids; unsigned n;
        if (XQueryTree(d, root, &r, &p, &kids, &n))
            for (unsigned i = 0; i < n; i++) {
                XClassHint ch; if (!XGetClassHint(d, kids[i], &ch)) continue;
                int hit = ch.res_class && !strcmp(ch.res_class, argv[2]);
                XFree(ch.res_name); XFree(ch.res_class); if (!hit) continue;
                XWindowAttributes a; XGetWindowAttributes(d, kids[i], &a);
                printf("0x%lx %d %d %d %d %d\n", kids[i], a.map_state == IsViewable, a.x, a.y, a.width, a.height);
            }
        if (kids) XFree(kids);
    } else if (argc >= 5 && !strcmp(argv[1], "pixel")) {
        Window w = strtoul(argv[2], NULL, 0);
        XImage *im = XGetImage(d, w, atoi(argv[3]), atoi(argv[4]), 1, 1, AllPlanes, ZPixmap);
        if (!im) return 1;
        printf("%06lx\n", XGetPixel(im, 0, 0) & 0xffffff); XDestroyImage(im);
    } else if (argc >= 4 && !strcmp(argv[1], "count")) {
        Window w = strtoul(argv[2], NULL, 0); unsigned long want = strtoul(argv[3], NULL, 16), cnt = 0;
        XWindowAttributes a; if (!XGetWindowAttributes(d, w, &a)) return 1;
        XImage *im = XGetImage(d, w, 0, 0, (unsigned)a.width, (unsigned)a.height, AllPlanes, ZPixmap);
        if (!im) return 1;
        for (int y = 0; y < a.height; y++) for (int x = 0; x < a.width; x++) if ((XGetPixel(im, x, y) & 0xffffff) == want) cnt++;
        printf("%lu\n", cnt); XDestroyImage(im);
    } else if (argc >= 5 && !strcmp(argv[1], "click")) {
        Window w = strtoul(argv[2], NULL, 0);
        for (int press = 1; press >= 0; press--) {
            XEvent e; memset(&e, 0, sizeof e);
            e.xbutton.type = press ? ButtonPress : ButtonRelease; e.xbutton.window = w; e.xbutton.root = DefaultRootWindow(d);
            e.xbutton.x = atoi(argv[3]); e.xbutton.y = atoi(argv[4]); e.xbutton.button = Button1; e.xbutton.same_screen = True;
            XSendEvent(d, w, True, press ? ButtonPressMask : ButtonReleaseMask, &e);
        }
        XSync(d, False);
    } else if (!strcmp(argv[1], "key") && argc >= 4) {
        Window w = strtoul(argv[2], NULL, 0); KeySym ks = XStringToKeysym(argv[3]); if (ks == NoSymbol) return 2;
        for (int press = 1; press >= 0; press--) {
            XEvent e; memset(&e, 0, sizeof e);
            e.xkey.type = press ? KeyPress : KeyRelease; e.xkey.window = w; e.xkey.root = DefaultRootWindow(d);
            e.xkey.keycode = XKeysymToKeycode(d, ks); e.xkey.same_screen = True;
            XSendEvent(d, w, True, press ? KeyPressMask : KeyReleaseMask, &e);
        }
        XSync(d, False);
    } else rc = 2;
    XCloseDisplay(d); return rc;
}
