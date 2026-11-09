#define _DEFAULT_SOURCE
/* Test helper: stands in for an EWMH window manager. Creates a client window with the given
 * WM_CLASS/title, adds it to the root's _NET_CLIENT_LIST, lives for N ms, then removes it. */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 5) return 2;
    Display *d = XOpenDisplay(NULL); if (!d) return 1;
    Window root = DefaultRootWindow(d), w = XCreateSimpleWindow(d, root, 0, 0, 50, 50, 0, 0, 0);
    XClassHint ch = { argv[1], argv[2] }; XSetClassHint(d, w, &ch);
    XStoreName(d, w, argv[3]);
    Atom list = XInternAtom(d, "_NET_CLIENT_LIST", False);
    XChangeProperty(d, root, list, XA_WINDOW, 32, PropModeAppend, (unsigned char *)&w, 1); XSync(d, False);
    usleep((useconds_t)atoi(argv[4]) * 1000);
    unsigned char *data; Atom t; int f; unsigned long n, a;
    XGetWindowProperty(d, root, list, 0, 4096, False, XA_WINDOW, &t, &f, &n, &a, &data);
    Window keep[256]; unsigned long k = 0;
    for (unsigned long i = 0; i < n && k < 256; i++) if (((Window *)data)[i] != w) keep[k++] = ((Window *)data)[i];
    XChangeProperty(d, root, list, XA_WINDOW, 32, PropModeReplace, (unsigned char *)keep, (int)k);
    if (data) XFree(data);
    XDestroyWindow(d, w); XSync(d, False); XCloseDisplay(d); return 0;
}
