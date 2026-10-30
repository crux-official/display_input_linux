// The libX11 symbol table (ticket 0027 / 0030): the 33 symbols the two
// x11 backends (display + input) reference. X-macro shape: included three
// times (probe.h / probe.c / trampoline_x11.h). libX11.so.6 has carried
// this stable API since the 1990s; the 0030 matrix confirms the floor
// (the oldest in-range distro where every one of these resolves).

#ifndef X11_SYM
#define X11_SYM(rc, fn, params)
#endif

X11_SYM(Display *, XOpenDisplay, (const char *display))
X11_SYM(void, XCloseDisplay, (Display *dpy))
X11_SYM(int, XFlush, (Display *dpy))
X11_SYM(int, XPending, (Display *dpy))
X11_SYM(int, XNextEvent, (Display *dpy, XEvent *event_return))
X11_SYM(int, XSendEvent, (Display *display, Window dest_window, int propagate, long event_mask, XEvent *event_send))
X11_SYM(Window, XCreateWindow, (Display *dpy, Window w, int x, int y, unsigned int width, unsigned int height, unsigned int border_width, int depth, int class, Visual *visual, unsigned long valuemask, XSetWindowAttributes *attributes))
X11_SYM(Status, XDestroyWindow, (Display *dpy, Window w))
X11_SYM(Status, XMapWindow, (Display *dpy, Window w))
X11_SYM(Status, XUnmapWindow, (Display *dpy, Window w))
X11_SYM(Status, XIconifyWindow, (Display *dpy, Window w, int screen))
X11_SYM(Status, XRaiseWindow, (Display *dpy, Window w))
X11_SYM(Status, XMoveWindow, (Display *dpy, Window w, int x, int y))
X11_SYM(Status, XResizeWindow, (Display *dpy, Window w, unsigned int width, unsigned int height))
X11_SYM(Status, XMoveResizeWindow, (Display *dpy, Window w, int x, int y, unsigned int width, unsigned int height))
X11_SYM(Status, XStoreName, (Display *display, Window w, const char *window_name))
X11_SYM(Status, XSetWMNormalHints, (Display *display, Window w, XSizeHints *size_hints))
X11_SYM(Status, XSetWMProtocols, (Display *display, Window w, Atom *protocols, int count))
X11_SYM(Atom, XInternAtom, (Display *display, const char *atom_name, int only_if_exists))
X11_SYM(Status, XChangeProperty, (Display *display, Window w, Atom property, Atom type, int format, int mode, unsigned char *data, int nelements))
X11_SYM(Status, XGetWindowProperty, (Display *dpy, Window w, Atom property, long long_offset, long long_length, int delete_property, Atom req_type, Atom *actual_return_type, int *actual_format, unsigned long *nitems, unsigned long *bytes_after, unsigned char **prop_return))
X11_SYM(XSizeHints *, XAllocSizeHints, (void))
X11_SYM(void, XFree, (void *ptr))
X11_SYM(KeyCode, XKeysymToKeycode, (Display *dpy, KeySym keysym))
X11_SYM(int, XQueryKeymap, (Display *dpy, char keys_values[32]))
X11_SYM(XVisualInfo *, XGetVisualInfo, (Display *dpy, long valuemask, XVisualInfo *template, int *nitems_return))
X11_SYM(VisualID, XVisualIDFromVisual, (Visual *visual))
X11_SYM(Status, XGetWindowAttributes, (Display *dpy, Window w, XWindowAttributes *wattr_return))
X11_SYM(GC, XCreateGC, (Display *dpy, Drawable d, unsigned long valuemask, XGCValues *values))
X11_SYM(void, XFreeGC, (Display *dpy, GC gc))
X11_SYM(XImage *, XCreateImage, (Display *dpy, Visual *visual, unsigned int depth, int format, int offset, char *data, unsigned int width, unsigned int height, int bitmap_pad, int bytes_per_line))
X11_SYM(void, XDestroyImage, (XImage *image))
X11_SYM(int, XPutImage, (Display *dpy, Drawable d, GC gc, XImage *image, int src_x, int src_y, int dest_x, int dest_y, unsigned int width, unsigned int height))
// The XKB extension (shipped inside libX11.so.6 on modern Xorg): the
// input sampler's named-indicator mods (caps/num) + the extension probe.
X11_SYM(int, XkbUseExtension, (Display *dpy, int *major_rtrn, int *minor_rtrn))
X11_SYM(Bool, XkbGetNamedIndicator, (Display *dpy, Atom name, int *pNdxRtrn, Bool *pStateRtrn, void *pMapRtrn, Bool *pRealRtrn))

#undef X11_SYM
