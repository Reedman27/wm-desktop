#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
/* Test helper: inspect and poke top-level X windows without xdotool.
 *   xprobe find CLASS            one line per mapped-or-not root child with that WM_CLASS class: "id mapped x y w h"
 *   xprobe pixel ID X Y          colour of one pixel as rrggbb
 *   xprobe count ID RRGGBB       how many pixels of that colour the window has
 *   xprobe click ID X Y [BTN[+ctrl][+shift]]   synthetic click (default left) at window coordinates
 *   xprobe drag ID X1 Y1 X2 Y2 [ctrl]          left press at 1, motion, release at 2
 *   xprobe enter|leave ID        synthetic pointer enter / leave (hover)
 *   xprobe key ID KEYSYM         synthetic key press + release, e.g. Down, q, ctrl+c */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <unistd.h>
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
    } else if (argc >= 5 && (!strcmp(argv[1], "click") || (argc >= 7 && !strcmp(argv[1], "drag")))) {
        /* click ID X Y [BUTTON[+ctrl][+shift]]   |   drag ID X1 Y1 X2 Y2 [ctrl]  (button 1 down, motion, release) */
        Window w = strtoul(argv[2], NULL, 0); int drag = !strcmp(argv[1], "drag"), x1 = atoi(argv[3]), y1 = atoi(argv[4]);
        int x2 = drag ? atoi(argv[5]) : x1, y2 = drag ? atoi(argv[6]) : y1, button = Button1; unsigned mods = 0;
        const char *opt = drag ? (argc > 7 ? argv[7] : "") : (argc > 5 ? argv[5] : "");
        if (!drag && *opt) button = atoi(opt);
        if (strstr(opt, "ctrl")) mods |= ControlMask;
        if (strstr(opt, "shift")) mods |= ShiftMask;
        int ox = 0, oy = 0; Window dummy; XTranslateCoordinates(d, w, DefaultRootWindow(d), 0, 0, &ox, &oy, &dummy);
        for (int step = 0; step < (drag ? 3 : 2); step++) {
            XEvent e; memset(&e, 0, sizeof e); int press = step == 0, motion = drag && step == 1, px = (drag && step > 0) ? x2 : x1, py = (drag && step > 0) ? y2 : y1;
            if (motion) {
                e.xmotion.type = MotionNotify; e.xmotion.window = w; e.xmotion.root = DefaultRootWindow(d); e.xmotion.x = px; e.xmotion.y = py;
                e.xmotion.x_root = ox + px; e.xmotion.y_root = oy + py; e.xmotion.state = Button1Mask | mods; e.xmotion.same_screen = True;
                XSendEvent(d, w, True, ButtonMotionMask | Button1MotionMask, &e);
            } else {
                e.xbutton.type = press ? ButtonPress : ButtonRelease; e.xbutton.window = w; e.xbutton.root = DefaultRootWindow(d);
                e.xbutton.x = px; e.xbutton.y = py; e.xbutton.x_root = ox + px; e.xbutton.y_root = oy + py; e.xbutton.button = (unsigned)button;
                e.xbutton.state = (drag && press) ? 0 : mods; e.xbutton.same_screen = True;
                XSendEvent(d, w, True, press ? ButtonPressMask : ButtonReleaseMask, &e);
            }
            XSync(d, False); usleep(60000);
        }
        XSync(d, False);
    } else if (argc >= 3 && (!strcmp(argv[1], "enter") || !strcmp(argv[1], "leave"))) {
        Window w = strtoul(argv[2], NULL, 0); XEvent e; memset(&e, 0, sizeof e); int in = !strcmp(argv[1], "enter");
        e.xcrossing.type = in ? EnterNotify : LeaveNotify; e.xcrossing.window = w; e.xcrossing.root = DefaultRootWindow(d);
        e.xcrossing.mode = NotifyNormal; e.xcrossing.detail = NotifyAncestor; e.xcrossing.same_screen = True;
        XSendEvent(d, w, True, in ? EnterWindowMask : LeaveWindowMask, &e); XSync(d, False);
    } else if (!strcmp(argv[1], "key") && argc >= 4) {
        Window w = strtoul(argv[2], NULL, 0); const char *kn = argv[3]; unsigned mods = 0;
        if (!strncmp(kn, "ctrl+", 5)) { mods = ControlMask; kn += 5; }
        KeySym ks = XStringToKeysym(kn); if (ks == NoSymbol) return 2;
        for (int press = 1; press >= 0; press--) {
            XEvent e; memset(&e, 0, sizeof e);
            e.xkey.type = press ? KeyPress : KeyRelease; e.xkey.window = w; e.xkey.root = DefaultRootWindow(d);
            e.xkey.keycode = XKeysymToKeycode(d, ks); e.xkey.same_screen = True; e.xkey.state = mods;
            XSendEvent(d, w, True, press ? KeyPressMask : KeyReleaseMask, &e);
        }
        XSync(d, False);
    } else rc = 2;
    XCloseDisplay(d); return rc;
}
