// The X11 display driver: windows + ARGB8888 framebuffers. The driver owns the Xlib connection (one Display*,
// opened lazily on the first CreateWindow - the server is $DISPLAY; a failed open is remembered, no reconnect),
// the Window handles, the framebuffers (the driver's allocated ARGB buffers, one XImage header each), and the
// event pump (the feed hook). A window is a mapped X window over a ZPixmap XImage: the pixels the app renders
// into ARE the XImage's data (zero-copy present - UpdateWindowFramebuffer XPutImages the image straight onto
// the window). A failed open is a clean degradation (one log line): CreateWindow is NULL, GetDisplayBounds
// reports zeros, the framebuffer functions fail, and the feed hook returns at once.
//
// The display driver IS the input source: for the input-relevant events the switch forwards the raw facts to
// the input driver through CruxInput_ForwardX11Event (it normalizes and pushes the neutral events); each Window
// is registered in the input driver's registry so the forwarder resolves Window -> window id. The KEY forward
// carries keycode + down + repeat only - the input driver samples the modifier state itself.
//
// The window states ride the WM's protocols: maximize/restore and fullscreen are the EWMH _NET_WM_STATE
// (sent when the WM advertises _NET_WM_STATE on the root's _NET_SUPPORTED; the move+size-to-the-screen is the
// fallback for a non-EWMH WM), minimize is the ICCCM XIconifyWindow, borderless is the _MOTIF_WM_HINTS
// decorations hint (the WM owns the decorations either way - the hint asks, it does not command), a
// non-resizable window is a min=max=size hint pair, and a minimum size is the WM_NORMAL_HINTS PMinSize (the
// WM clamps the user's resize against it; SetWindowSize clamps to the same floor). The window's _NET_WM_STATE
// property is tracked (PropertyNotify): the WM is the authority, the user's own title-bar clicks included.
//
// Scope: screen 0's default visual (TrueColor at depth 24 or 32 - the ARGB8888 XImage floor, checked at
// connect), the params are the client size as-is (the WM decorates around the window), the present is a full
// XPutImage (the dirty rects are accepted and ignored), the unhandled event types are dropped with a one-time
// note, and a user-initiated minimize raises no event (the map state rides the UnmapNotify, which the contract
// has no kind for).
//
// The framebuffer resizes WITH the window: on a ConfigureNotify (a user resize, a state change the WM honored,
// or a SetWindowSize) the driver reallocates the backing buffer, preserving the overlap (top-left anchored, no
// scaling) - the app's last frame survives. The pixels/pitch the app holds are only valid until the next size
// change: after the WINDOW_RESIZE event the app re-calls CreateWindowFramebuffer (a metadata update, not a
// re-allocation).

#include <poll.h>
#include "kernel_types.h"
#include "logging.h"
#include "display.h"
#include "display_api.h"
#include "memory.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h> // the XImage teardown (XDestroyImage), the size hints
#include <X11/Xatom.h> // XA_ATOM (the _NET_* property types)

// The type headers first (their real prototypes are dead - nothing links
// libX11), then the rename layer (every call site below hits the
// dlopen'd table).
#include "trampoline_x11.h"

// The contract rename: this backend's Display_* implementations carry a
// per-backend prefix (the served DisplayAPI lives in display.c - ONE per
// binary); the dispatch on the probed winner is in display.c too.
#define Display_GetDisplayCount DlX11_GetDisplayCount
#define Display_GetDisplayBounds DlX11_GetDisplayBounds
#define Display_CreateWindow DlX11_CreateWindow
#define Display_DestroyWindow DlX11_DestroyWindow
#define Display_SetWindowTitle DlX11_SetWindowTitle
#define Display_SetWindowPosition DlX11_SetWindowPosition
#define Display_SetWindowSize DlX11_SetWindowSize
#define Display_SetWindowMinSize DlX11_SetWindowMinSize
#define Display_GetWindowSizeInPixels DlX11_GetWindowSizeInPixels
#define Display_ShowWindow DlX11_ShowWindow
#define Display_HideWindow DlX11_HideWindow
#define Display_RaiseWindow DlX11_RaiseWindow
#define Display_MinimizeWindow DlX11_MinimizeWindow
#define Display_MaximizeWindow DlX11_MaximizeWindow
#define Display_RestoreWindow DlX11_RestoreWindow
#define Display_SetWindowFullscreen DlX11_SetWindowFullscreen
#define Display_CreateWindowFramebuffer DlX11_CreateWindowFramebuffer
#define Display_DestroyWindowFramebuffer DlX11_DestroyWindowFramebuffer
#define Display_UpdateWindowFramebuffer DlX11_UpdateWindowFramebuffer



// --- Cross-driver symbols (the input driver's queue + registry) -----------------
// The display driver is the event source, the input driver queues (the header carries the contract).
void CruxInput_RegisterWindow(void *os_window, uint32_t window_id);
void CruxInput_UnregisterWindow(void *os_window);
void CruxInput_SetFeedHook(void (*feed)(void *ud, uint64_t timeoutNS), void *ud);
void CruxInput_ForwardX11Event(void *os_window, uint32_t kind, int32_t x, int32_t y, uint32_t code, uint32_t button, bool down, int32_t wheelX, int32_t wheelY, bool repeat);

// The forwarder's kind values (the input driver holds the identical set as IX_EVT_*; keep the values in sync):
#define DX_EVT_MOTION 0      // x,y window coords, y-down
#define DX_EVT_BUTTON 1      // button 1-based, down
#define DX_EVT_WHEEL  2      // wheelX/wheelY notches
#define DX_EVT_KEY    3      // code = the X11 keycode, down, repeat
#define DX_EVT_FOCUS  4      // down = focused
#define DX_EVT_RESIZE 5      // x,y = new client w,h
#define DX_EVT_CLOSE  6

// --- Constants --------------------------------------------------------------------

#define DX_MAX_WINDOWS 16
#define DX_FORMAT_ARGB8888 0x16362004u // the ARGB, 32-bps layout tag
#define DX_DEFAULT_W 640               // the params' default client size
#define DX_DEFAULT_H 480
#define DX_KEYS 256                    // the X11 keycode range (the OS-repeat down table)

// The event mask the driver selects on every window (in force from
// creation, the XSelectInput set): the input-relevant events the switch
// forwards + Structure (the resize/close facts) + Focus.
#define DX_EVENT_MASK \
  (ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | \
   ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | FocusChangeMask | PropertyChangeMask)

// --- The window ---------------------------------------------------------------------

typedef struct DxWindow {
  bool in_use;
  uint32_t window_id; // driver-assigned, 1-based (0 is the contract's "no window")
  int32_t x, y;       // the window position (the last ConfigureNotify; before one, the requested)
  int32_t w, h;       // the current client size (the last ConfigureNotify; before one, the requested)
  int32_t saved_w, saved_h; // the last CreateWindow/SetWindowSize size (the non-EWMH restore target)
  int32_t min_w, min_h;     // the min-size floor (SetWindowMinSize; 0/0 = the driver's default floor)
  bool maximized;           // tracked from the window's _NET_WM_STATE property (the WM is the authority)
  bool fullscreen;
  Window win;         // dead once XDestroyWindow has run (None distinguishes)
  GC gc;              // the window's graphics context (default values, the present's target)
  // The ARGB framebuffer (the app's pixels): the driver's allocated buffer
  // wrapped in an XImage header (Xlib-allocated; XDestroyImage frees the
  // header only, the pixels stay the driver's).
  XImage *image; // NULL = no framebuffer
  void *pixels;  // the buffer (the pointer handed to the app)
  int32_t pitch; // w * 4
  uint32_t format;
} DxWindow;

// --- The driver state ------------------------------------------------------------------

typedef struct DxState {
  bool connect_attempted; // the XOpenDisplay attempt (the result is remembered; no retry)
  Display *dpy;           // NULL = no connection (no X socket, or an unmet visual floor)
  int screen;             // screen 0 (the primary)
  int depth;              // the default visual's depth (the XImage's depth)
  Visual *visual;         // the default visual of screen 0 (TrueColor 32bpp, checked at connect)
  Colormap colormap;      // the default colormap (no colormap management)
  Atom wmProtocolsAtom;   // "WM_PROTOCOLS" (the close request's ClientMessage message_type)
  Atom wmDeleteAtom;      // "WM_DELETE_WINDOW" (the close request's protocol)
  Atom netSupportedAtom;  // "_NET_SUPPORTED" (the root's property: the WM's advertised atoms)
  Atom netWmStateAtom;    // "_NET_WM_STATE" (the state message's type / the window's state property)
  Atom netMaxVertAtom;    // "_NET_WM_STATE_MAXIMIZED_VERT"
  Atom netMaxHorzAtom;    // "_NET_WM_STATE_MAXIMIZED_HORZ"
  Atom netFullAtom;       // "_NET_WM_STATE_FULLSCREEN"
  Atom motifHintsAtom;    // "_MOTIF_WM_HINTS" (the decorations hint)
  bool ewmh;              // the WM advertises _NET_WM_STATE (the root's _NET_SUPPORTED says so)
  bool feed_registered;   // the feed hook handed to the input driver (once)
  uint32_t next_window_id;// 1-based
  uint8_t keys_down[DX_KEYS]; // per-keycode OS-repeat tracking: a second KeyPress
                              // before the KeyRelease is a repeat
  uint32_t noted_types;   // the one-time drop notes (bit N = event type N noted)
  DxWindow *windows[DX_MAX_WINDOWS];
} DxState;
static DxState s;

// --- The window lookups ---------------------------------------------------------------------

static DxWindow *dx_findWindow(void *handle) {
  if (handle == NULL) {
    return NULL;
  }
  for (int i = 0; i < DX_MAX_WINDOWS; i++) {
    if (s.windows[i] != NULL && s.windows[i]->in_use && s.windows[i] == handle) {
      return s.windows[i];
    }
  }
  return NULL;
}

// The record by its X Window (the event switch's lookup: the events
// carry the Window, not the record).
static DxWindow *dx_findByXWindow(Window win) {
  for (int i = 0; i < DX_MAX_WINDOWS; i++) {
    if (s.windows[i] != NULL && s.windows[i]->in_use && s.windows[i]->win == win) {
      return s.windows[i];
    }
  }
  return NULL;
}

// --- The connection (opened lazily once) ---------------------------------------------------

// The XOpenDisplay attempt, remembered (no reconnect). The visual floor is checked at connect (TrueColor at
// depth 24 or 32, below); an unmet floor is the same clean degradation as a failed open.
static bool dx_ensureDisplay(void) {
  if (s.connect_attempted) {
    return s.dpy != NULL;
  }
  s.connect_attempted = true;
  Display *dpy = XOpenDisplay(NULL); // the $DISPLAY server
  if (dpy == NULL) {
    LogError("[display_x11] XOpenDisplay failed (no reachable X server on $DISPLAY)");
    return false;
  }
  const int screen = DefaultScreen(dpy);
  const int depth = DefaultDepth(dpy, screen);
  Visual *visual = DefaultVisual(dpy, screen);
  // The ARGB8888 floor: the default visual must be TrueColor at depth 24 or 32 - the two TrueColor forms whose
  // ZPixmap pixels are 32 bits wide (the standard X11 "32-bit" screen is depth 24, the 4th byte being the
  // padding byte the ARGB8888 value's alpha rides in; depth 32 is the rarer all-alpha form). 16-bit TrueColor
  // is refused (its pixels are not a byte-aligned ARGB8888). The class comes from XGetVisualInfo (the portable
  // spelling).
  XVisualInfo query;
  int count = 0;
  query.visualid = XVisualIDFromVisual(visual);
  XVisualInfo *infos = XGetVisualInfo(dpy, VisualIDMask, &query, &count);
  if (count != 1 || infos->class != TrueColor || (depth != 24 && depth != 32)) {
    LogError("[display_x11] the default visual is not TrueColor at depth 24/32 (depth %d); the ARGB8888 floor is not met",
                      depth);
    if (infos != NULL) {
      XFree(infos);
    }
    XCloseDisplay(dpy);
    return false;
  }
  XFree(infos);
  s.dpy = dpy;
  s.screen = screen;
  s.depth = depth;
  s.visual = visual;
  s.colormap = DefaultColormap(dpy, screen);
  s.wmProtocolsAtom = XInternAtom(dpy, "WM_PROTOCOLS", False);
  s.wmDeleteAtom = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  s.netSupportedAtom = XInternAtom(dpy, "_NET_SUPPORTED", False);
  s.netWmStateAtom = XInternAtom(dpy, "_NET_WM_STATE", False);
  s.netMaxVertAtom = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
  s.netMaxHorzAtom = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
  s.netFullAtom = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
  s.motifHintsAtom = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
  // The WM's advertised set (the root's _NET_SUPPORTED): the EWMH state messages only when the WM says it
  // honors them - the fallback geometry is the honest path for a WM that does not.
  s.ewmh = false;
  if (s.netWmStateAtom != None) {
    Atom *supported = NULL;
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    if (XGetWindowProperty(dpy, RootWindow(dpy, screen), s.netSupportedAtom, 0, 256, False, XA_ATOM,
                           &actualType, &actualFormat, &nitems, &bytesAfter,
                           (unsigned char **)&supported) == Success &&
        actualType == XA_ATOM && actualFormat == 32) {
      for (unsigned long i = 0; i < nitems; i++) {
        if (supported[i] == s.netWmStateAtom) {
          s.ewmh = true;
        }
      }
    }
    if (supported != NULL) {
      XFree(supported);
    }
  }
  return true;
}

// --- The window teardown (shared by both destroy paths) -----------------------------------

// Every release in one place (both destroy paths): the input registry entry first (after this a forward for
// this Window resolves to nothing and is dropped), the X window, the GC, the XImage header (the pixels are
// the driver's, freed below), the pixels, the record.
static void dx_releaseWindow(DxWindow *w) {
  CruxInput_UnregisterWindow((void *)(uintptr_t)w->win);
  if (s.dpy != NULL) {
    XDestroyWindow(s.dpy, w->win);
    if (w->gc != None) {
      XFreeGC(s.dpy, w->gc);
      w->gc = None;
    }
    if (w->image != NULL) {
      w->image->data = NULL; // this Xlib's XDestroyImage frees data; the pixels free below is the driver's
      XDestroyImage(w->image);
      w->image = NULL;
    }
  }
  if (w->pixels != NULL) {
    Memory_Free(w->pixels);
    w->pixels = NULL;
  }
  w->pitch = 0;
  w->format = 0;
  w->win = None;
  w->in_use = false;
  for (int i = 0; i < DX_MAX_WINDOWS; i++) {
    if (s.windows[i] == w) {
      s.windows[i] = NULL;
    }
  }
  Memory_Free(w);
}

// The resize helper the ConfigureNotify handler reaches before its definition (the smart-resize contract).
static void dx_resizeFramebuffer(DxWindow *w);

// --- The event switch (the input source: forward raw, own the structure) ------------------

// One event, one place: the input-relevant events forward raw to the input driver (it normalizes); the
// structural events the driver owns (the record's size, the close); everything else is dropped (a one-time
// log note per type).
static void dx_handleEvent(const XEvent *ev) {
  switch (ev->type) {
    // --- The input forwards (the input driver normalizes) ---
    case KeyPress:
    case KeyRelease: {
      // X11 has no per-event repeat flag: an OS repeat is another KeyPress for the still-pressed keycode - the
      // per-keycode down table turns the first press into repeat=false and the held ones into repeat=true.
      const uint32_t keycode = ev->xkey.keycode;
      if (keycode < DX_KEYS) {
        const bool down = (ev->type == KeyPress);
        bool repeat = false;
        if (down) {
          repeat = s.keys_down[keycode] != 0;
          s.keys_down[keycode] = 1;
        } else {
          s.keys_down[keycode] = 0;
        }
        CruxInput_ForwardX11Event((void *)(uintptr_t)ev->xkey.window, DX_EVT_KEY, 0, 0, keycode, 0, down, 0, 0, repeat);
      }
      break;
    }
    case ButtonPress:
    case ButtonRelease: {
      // The X11 button numbers are already 1-based (1=left 2=middle 3=right). The wheel rides buttons 4..7:
      // 4/5 = up/down (wheelY, positive-up), 6/7 = left/right (positive-right: 7 -> +1, 6 -> -1).
      const bool down = (ev->type == ButtonPress);
      const int32_t x = ev->xbutton.x;
      const int32_t y = ev->xbutton.y;
      void *os_window = (void *)(uintptr_t)ev->xbutton.window;
      switch (ev->xbutton.button) {
        case 1:
        case 2:
        case 3:
          CruxInput_ForwardX11Event(os_window, DX_EVT_BUTTON, x, y, 0, ev->xbutton.button, down, 0, 0, false);
          break;
        case 4: // wheel up
          CruxInput_ForwardX11Event(os_window, DX_EVT_WHEEL, x, y, 0, 0, false, 0, 1, false);
          break;
        case 5: // wheel down
          CruxInput_ForwardX11Event(os_window, DX_EVT_WHEEL, x, y, 0, 0, false, 0, -1, false);
          break;
        case 6: // wheel left
          CruxInput_ForwardX11Event(os_window, DX_EVT_WHEEL, x, y, 0, 0, false, -1, 0, false);
          break;
        case 7: // wheel right
          CruxInput_ForwardX11Event(os_window, DX_EVT_WHEEL, x, y, 0, 0, false, 1, 0, false);
          break;
        default:
          break; // extra buttons (8+): no BUTTON kind for them - dropped
      }
      break;
    }
    case MotionNotify:
      // x,y are window coords, y-down (X11's native orientation is the contract's).
      CruxInput_ForwardX11Event((void *)(uintptr_t)ev->xmotion.window, DX_EVT_MOTION, ev->xmotion.x, ev->xmotion.y, 0, 0, false, 0, 0, false);
      break;
    case FocusIn:
      CruxInput_ForwardX11Event((void *)(uintptr_t)ev->xfocus.window, DX_EVT_FOCUS, 0, 0, 0, 0, true, 0, 0, false);
      break;
    case FocusOut:
      CruxInput_ForwardX11Event((void *)(uintptr_t)ev->xfocus.window, DX_EVT_FOCUS, 0, 0, 0, 0, false, 0, 0, false);
      break;
    // --- The structural events (the driver owns these) ---
    case ConfigureNotify: {
      // The server's new placement: the record owns the x,y,w,h. A size change resizes the framebuffer WITH
      // the window (the smart-resize contract: the overlap preserved, the app re-fetches on the event).
      DxWindow *w = dx_findByXWindow(ev->xconfigure.window);
      if (w != NULL) {
        w->x = ev->xconfigure.x;
        w->y = ev->xconfigure.y;
        if (w->w != ev->xconfigure.width || w->h != ev->xconfigure.height) {
          const bool hadFb = w->image != NULL;
          w->w = ev->xconfigure.width;
          w->h = ev->xconfigure.height;
          if (hadFb) {
            dx_resizeFramebuffer(w); // the realloc (the overlap preserved) + the XImage rebuild
          }
          CruxInput_ForwardX11Event((void *)(uintptr_t)w->win, DX_EVT_RESIZE, w->w, w->h, 0, 0, false, 0, 0, false);
        }
      }
      break;
    }
    case MappingNotify:
    case UnmapNotify:
      // No map event kind in the contract - dropped (the show/hide state is the app's bookkeeping; a
      // user-initiated minimize rides the UnmapNotify and raises no event).
      break;
    case PropertyNotify: {
      // The window's _NET_WM_STATE property: the WM's answer to a state request - or the user's own
      // title-bar click. The WM is the authority, so the record's state follows the property.
      if (s.netWmStateAtom != None && ev->xproperty.atom == s.netWmStateAtom) {
        DxWindow *w = dx_findByXWindow(ev->xproperty.window);
        if (w != NULL) {
          w->maximized = false;
          w->fullscreen = false;
          if (ev->xproperty.state == PropertyNewValue) {
            Atom *states = NULL;
            Atom actualType = None;
            int actualFormat = 0;
            unsigned long nitems = 0, bytesAfter = 0;
            if (XGetWindowProperty(s.dpy, w->win, s.netWmStateAtom, 0, 16, False, XA_ATOM, &actualType,
                                   &actualFormat, &nitems, &bytesAfter,
                                   (unsigned char **)&states) == Success &&
                actualType == XA_ATOM && actualFormat == 32) {
              for (unsigned long i = 0; i < nitems; i++) {
                if (states[i] == s.netMaxVertAtom || states[i] == s.netMaxHorzAtom) {
                  w->maximized = true;
                } else if (states[i] == s.netFullAtom) {
                  w->fullscreen = true;
                }
              }
            }
            if (states != NULL) {
              XFree(states);
            }
          }
        }
      }
      break;
    }
    case ClientMessage: {
      // The WM_DELETE_WINDOW close request (ICCCM: message_type is WM_PROTOCOLS, the protocol in data.l[0]):
      // the close event forwards (the registry entry is still live) and the window tears down.
      if (s.wmDeleteAtom != None && ev->xclient.message_type == s.wmProtocolsAtom && (Atom)ev->xclient.data.l[0] == s.wmDeleteAtom) {
        DxWindow *w = dx_findByXWindow(ev->xclient.window);
        if (w != NULL) {
          CruxInput_ForwardX11Event((void *)(uintptr_t)w->win, DX_EVT_CLOSE, 0, 0, 0, 0, false, 0, 0, false);
          dx_releaseWindow(w);
        }
      }
      break;
    }
    default: {
      // Expose, GravityNotify, PropertyNotify, ...: the rest are dropped - a one-time log note per type marks what arrived.
      const unsigned int t = (unsigned int)ev->type;
      if (t < 32 && (s.noted_types & (1u << t)) == 0) {
        s.noted_types |= 1u << t;
        Log("[display_x11] dropping event type %u (not handled)", t);
      }
      break;
    }
  }
}

// --- The feed hook (the input driver's wait drains the connection) -------------------------

// The event pump: drain what's queued, then wait on the connection's fd with the ring's remaining budget -
// poll returns the moment an event arrives (the device-interruptible wait), a timeout ends the wait.
static void dx_feed(void *ud, uint64_t timeoutNS) {
  (void)ud;
  if (s.dpy == NULL) {
    return; // no display (no X socket): nothing to pump
  }
  XEvent ev;
  do {
    while (XPending(s.dpy) > 0) {
      XNextEvent(s.dpy, &ev);
      dx_handleEvent(&ev);
    }
    if (timeoutNS == 0) {
      break;
    }
    struct pollfd pfd = { ConnectionNumber(s.dpy), POLLIN, 0 };
    const long ms = timeoutNS > 4294967295ULL ? 4294967 : (long)(timeoutNS / 1000000ULL);
    // poll's ms is a 32-bit int: the clamp above caps a >4.29 s budget
    if (poll(&pfd, 1, ms) > 0) {
      continue; // readable: drain the next lap
    }
    break; // timeout: the input ring timed out
  } while (true);
}

// --- The window states + the framebuffer resize --------------------------------------------------------

// The EWMH _NET_WM_STATE client message (the WM is the audience: it is sent to the root, masked for the
// redirect so the WM sees it): the state is added (1) or removed (0).
static void dx_setWmState(DxWindow *w, Atom state, bool set) {
  XEvent ev;
  memset(&ev, 0, sizeof ev);
  ev.xclient.type = ClientMessage;
  ev.xclient.display = s.dpy;
  ev.xclient.window = w->win;
  ev.xclient.message_type = s.netWmStateAtom;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = set ? 1 : 0; // _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE
  ev.xclient.data.l[1] = (long)state;
  ev.xclient.data.l[2] = 0;
  XSendEvent(s.dpy, RootWindow(s.dpy, s.screen), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
  XFlush(s.dpy);
}

// The smart resize: the buffer's realloc to the window's current w,h (the overlap preserved - realloc's
// top-left anchoring; the grown region zeroed, a stale-pixel flash is not the contract), the XImage rebuild
// around the new buffer. A failed realloc keeps the old buffer (the present covers the old size until a retry).
static void dx_resizeFramebuffer(DxWindow *w) {
  const int32_t W = w->w > 0 ? w->w : 1;
  const int32_t H = w->h > 0 ? w->h : 1;
  const int32_t oldW = w->image != NULL ? (int32_t)w->image->width : 0;
  const int32_t oldH = w->image != NULL ? (int32_t)w->image->height : 0;
  const int32_t oldPitch = w->pitch;
  const size_t newBytes = (size_t)W * (size_t)H * 4u;
  // The overlap preserved ROW BY ROW: a raw realloc keeps the old bytes at the OLD pitch, and a width
  // change moves every row's offset - the overlap would smear. The new buffer is zeroed (the grown
  // region), then the overlap's rows land at their new offsets (top-left anchored, no scaling).
  uint8_t *newPixels = (uint8_t *)Memory_Alloc(newBytes);
  if (newPixels == NULL) {
    LogError("[display_x11] the framebuffer realloc failed (%dx%d)", W, H);
    return;
  }
  memset(newPixels, 0, newBytes);
  if (w->pixels != NULL && oldW > 0 && oldH > 0) {
    const int32_t copyW = W < oldW ? W : oldW;
    const int32_t copyH = H < oldH ? H : oldH;
    const size_t rowBytes = (size_t)copyW * 4u;
    for (int32_t row = 0; row < copyH; row++) {
      memcpy(newPixels + (size_t)row * (size_t)W * 4u,
             (const uint8_t *)w->pixels + (size_t)row * (size_t)oldPitch, rowBytes);
    }
  }
  Memory_Free(w->pixels);
  w->pixels = newPixels;
  if (w->image != NULL) {
    w->image->data = NULL; // this Xlib's XDestroyImage frees data; the buffer's lifecycle is the driver's
    XDestroyImage(w->image);
    w->image = NULL;
  }
  w->pitch = W * 4;
  w->image = XCreateImage(s.dpy, s.visual, s.depth, ZPixmap, 0, (char *)w->pixels,
                          (unsigned int)W, (unsigned int)H, 32, w->pitch);
  if (w->image == NULL) {
    LogError("[display_x11] the resize's XImage rebuild failed (%dx%d)", W, H);
  }
}

// --- The contract: the display concern ---------------------------------------------------------------

int32_t Display_GetDisplayCount(void) {
  return 1; // one display (the primary screen)
}

bool Display_GetDisplayBounds(int32_t display, CruxRect *bounds) {
  if (display != 0 || bounds == NULL) {
    return false;
  }
  // The primary screen only. No connection: zeros (the display exists in the contract; the geometry is
  // unknown without a server).
  bounds->x = 0;
  bounds->y = 0;
  if (s.dpy == NULL) {
    bounds->w = 0;
    bounds->h = 0;
    return true;
  }
  bounds->w = DisplayWidth(s.dpy, s.screen);
  bounds->h = DisplayHeight(s.dpy, s.screen);
  return true;
}

void *Display_CreateWindow(const CruxWindowParams *params) {
  if (params == NULL) {
    return NULL;
  }
  if (!dx_ensureDisplay()) {
    return NULL; // no X socket / an unmet visual floor (the error is logged)
  }
  if (!s.feed_registered) {
    // Hand the pump to the input driver's queue once (the first CreateWindow): its WaitEvent drains this connection.
    s.feed_registered = true;
    CruxInput_SetFeedHook(dx_feed, NULL);
  }
  DxWindow **slot = NULL;
  for (int i = 0; i < DX_MAX_WINDOWS; i++) {
    if (s.windows[i] == NULL) {
      slot = &s.windows[i];
      break;
    }
  }
  if (slot == NULL) {
    LogError("[display_x11] no free window slot (%d in use)", DX_MAX_WINDOWS);
    return NULL;
  }
  DxWindow *w = (DxWindow *)Memory_Calloc(1, sizeof *w);
  if (w == NULL) {
    LogError("[display_x11] the window record allocation failed");
    return NULL;
  }
  w->in_use = true;
  w->window_id = ++s.next_window_id; // 1-based (0 = no window, the contract's reserved value)
  w->x = params->x;
  w->y = params->y;
  w->w = params->w > 0 ? params->w : DX_DEFAULT_W;
  w->h = params->h > 0 ? params->h : DX_DEFAULT_H;
  w->saved_w = w->w;
  w->saved_h = w->h;
  // The params are the client size as-is: an X window has no frame to adjust (the WM decorates around the window).
  XSetWindowAttributes attrs;
  memset(&attrs, 0, sizeof attrs);
  attrs.background_pixel = 0; // black under a map/expose before the first present
  attrs.border_pixel = 0;
  attrs.colormap = s.colormap; // the default colormap (no management)
  attrs.event_mask = DX_EVENT_MASK;
  w->win = XCreateWindow(s.dpy, RootWindow(s.dpy, s.screen),
                         w->x, w->y, (unsigned int)w->w, (unsigned int)w->h, 0,
                         DefaultDepth(s.dpy, s.screen), InputOutput, s.visual,
                         CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &attrs);
  if (w->win == None) {
    LogError("[display_x11] XCreateWindow failed (%dx%d)", w->w, w->h);
    Memory_Free(w);
    return NULL;
  }
  w->gc = XCreateGC(s.dpy, w->win, 0, NULL); // the default-valued context (the present's target)
  if (w->gc == None) {
    LogError("[display_x11] XCreateGC failed");
    XDestroyWindow(s.dpy, w->win);
    Memory_Free(w);
    return NULL;
  }
  if (params->title != NULL && params->title[0] != '\0') {
    XStoreName(s.dpy, w->win, params->title);
  }
  // The WM close box (the title-bar X): the WM asks WM_DELETE_WINDOW,
  // the switch forwards CLOSE and tears the window down.
  if (s.wmProtocolsAtom != None && s.wmDeleteAtom != None) {
    Atom protocols[1] = { s.wmDeleteAtom };
    XSetWMProtocols(s.dpy, w->win, protocols, 1);
  }
  // The size hints: the floor the WM clamps the user's resize against (ICCCM). A non-resizable window is
  // fixed at its created size (min = max); a resizable one rides the driver's default floor, which
  // SetWindowMinSize moves.
  XSizeHints *hints = XAllocSizeHints();
  if (hints != NULL) {
    if (params->resizable) {
      w->min_w = 320;
      w->min_h = 240;
      hints->flags = PMinSize;
      hints->min_width = w->min_w;
      hints->min_height = w->min_h;
    } else {
      w->min_w = w->w;
      w->min_h = w->h;
      hints->flags = PMinSize | PMaxSize;
      hints->min_width = w->w;
      hints->min_height = w->h;
      hints->max_width = w->w;
      hints->max_height = w->h;
    }
    XSetWMNormalHints(s.dpy, w->win, hints);
    XFree(hints);
  }
  if (params->borderless && s.motifHintsAtom != None) {
    // The _MOTIF_WM_HINTS decorations hint (the WM's honor system for frame removal): the layout is the
    // classic {flags, functions, decorations, input_mode, status} — flags = MWM_HINTS_DECORATIONS (1<<1),
    // decorations = 0 (no frame). The WM owns the decorations either way; the hint asks.
    unsigned long motif[5] = { 2u, 0u, 0u, 0L, 0u };
    XChangeProperty(s.dpy, w->win, s.motifHintsAtom, s.motifHintsAtom, 32, PropModeReplace, (unsigned char *)motif, 5);
  }
  if (!params->hidden) {
    XMapWindow(s.dpy, w->win);
    XFlush(s.dpy);
  }
  *slot = w;
  CruxInput_RegisterWindow((void *)(uintptr_t)w->win, w->window_id);
  return w;
}

void Display_DestroyWindow(void *window) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL) {
    return; // already torn down (a double destroy)
  }
  // The app asked for the destroy: no close event (the registry entry drops first). The WM-close path arrives
  // through the event switch (its entry still live) and pushes its close event normally.
  dx_releaseWindow(w);
}

void Display_SetWindowTitle(void *window, const char *title) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL || title == NULL) {
    return;
  }
  XStoreName(s.dpy, w->win, title);
}

void Display_SetWindowPosition(void *window, int32_t x, int32_t y) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL) {
    return;
  }
  XMoveWindow(s.dpy, w->win, x, y);
  // The record's x,y refresh on the ConfigureNotify the XMoveWindow
  // queues (the feed hook dispatches it on the next pump).
}

void Display_SetWindowSize(void *window, int32_t w, int32_t h) {
  DxWindow *win = dx_findWindow(window);
  if (win == NULL || w <= 0 || h <= 0) {
    return;
  }
  if (win->min_w > 0 && w < win->min_w) {
    w = win->min_w; // the driver's own floor (the WM enforces it for the user's resize)
  }
  if (win->min_h > 0 && h < win->min_h) {
    h = win->min_h;
  }
  if (win->w == w && win->h == h) {
    return;
  }
  win->saved_w = w;
  win->saved_h = h;
  XResizeWindow(s.dpy, win->win, w, h);
  // The record + the framebuffer resize on the ConfigureNotify the XResizeWindow
  // queues (the feed hook dispatches it on the next pump).
}

void Display_SetWindowMinSize(void *window, int32_t minW, int32_t minH) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL || s.dpy == NULL) {
    return;
  }
  if (minW < 0) {
    minW = 0;
  }
  if (minH < 0) {
    minH = 0;
  }
  w->min_w = minW;
  w->min_h = minH;
  // The WM_NORMAL_HINTS PMinSize: the WM clamps the user's resize against it (ICCCM). 0/0 (clear) sends no
  // min hint; a 0 in one axis is an explicit no-limit for that axis.
  XSizeHints *hints = XAllocSizeHints();
  if (hints == NULL) {
    return;
  }
  if (minW == 0 && minH == 0) {
    hints->flags = 0; // the property with no flags: the min constraint is gone
  } else {
    hints->flags = PMinSize;
    hints->min_width = minW;
    hints->min_height = minH;
  }
  XSetWMNormalHints(s.dpy, w->win, hints);
  XFree(hints);
  XFlush(s.dpy);
}

void Display_GetWindowSizeInPixels(void *window, int32_t *w, int32_t *h) {
  DxWindow *win = dx_findWindow(window);
  if (win == NULL || w == NULL || h == NULL) {
    return;
  }
  XWindowAttributes attrs;
  if (XGetWindowAttributes(s.dpy, win->win, &attrs)) {
    *w = attrs.width;
    *h = attrs.height;
  }
}

void Display_ShowWindow(void *window) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL) {
    return;
  }
  XMapWindow(s.dpy, w->win);
  XFlush(s.dpy);
}

void Display_HideWindow(void *window) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL) {
    return;
  }
  XUnmapWindow(s.dpy, w->win);
  XFlush(s.dpy);
}

void Display_RaiseWindow(void *window) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL) {
    return;
  }
  XRaiseWindow(s.dpy, w->win);
}

void Display_MinimizeWindow(void *window) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL) {
    return;
  }
  XIconifyWindow(s.dpy, w->win, s.screen);
  XFlush(s.dpy);
}

void Display_MaximizeWindow(void *window) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL) {
    return;
  }
  if (s.ewmh) {
    // The EWMH way: the WM maximizes (and remembers the geometry it restores); the answer rides
    // the _NET_WM_STATE property (the PropertyNotify tracking) + the ConfigureNotify.
    dx_setWmState(w, s.netMaxVertAtom, true);
    dx_setWmState(w, s.netMaxHorzAtom, true);
  } else {
    // No EWMH: size the window to the screen's bounds (the record's saved size keeps the restore target).
    XMoveResizeWindow(s.dpy, w->win, 0, 0, DisplayWidth(s.dpy, s.screen), DisplayHeight(s.dpy, s.screen));
    XFlush(s.dpy);
  }
}

void Display_SetWindowFullscreen(void *window, bool fullscreen) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL) {
    return;
  }
  if (s.ewmh) {
    // The EWMH way: the WM owns the transition (the state + the size ride the property + the
    // ConfigureNotify; the framebuffer resizes WITH the window).
    dx_setWmState(w, s.netFullAtom, fullscreen);
  } else if (fullscreen) {
    XMoveResizeWindow(s.dpy, w->win, 0, 0, DisplayWidth(s.dpy, s.screen), DisplayHeight(s.dpy, s.screen));
    XFlush(s.dpy);
  } else {
    // The exit's fallback: the saved pair (the pre-fullscreen size).
    const int32_t rw = w->saved_w > 0 ? w->saved_w : DX_DEFAULT_W;
    const int32_t rh = w->saved_h > 0 ? w->saved_h : DX_DEFAULT_H;
    XMoveResizeWindow(s.dpy, w->win, 0, 0, rw, rh);
    XFlush(s.dpy);
  }
}

void Display_RestoreWindow(void *window) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL) {
    return;
  }
  if (s.ewmh) {
    // The WM restores the pre-state geometry itself: the states drop (the ConfigureNotify carries the
    // size back; the framebuffer resizes WITH the window). The fullscreen drop covers a window that
    // was fullscreened, not maximized (dropping an unset state is a no-op). The map covers a window
    // that was MINIMIZED (an iconified window comes back by mapping - the SW_RESTORE semantics: a
    // restore is a restore from any state; mapping a mapped window is a no-op).
    dx_setWmState(w, s.netMaxVertAtom, false);
    dx_setWmState(w, s.netMaxHorzAtom, false);
    dx_setWmState(w, s.netFullAtom, false);
    XMapWindow(s.dpy, w->win);
  } else {
    // Back to the last real size (the fallback maximize is a plain resize, so the saved pair is the target).
    const int32_t rw = w->saved_w > 0 ? w->saved_w : DX_DEFAULT_W;
    const int32_t rh = w->saved_h > 0 ? w->saved_h : DX_DEFAULT_H;
    XResizeWindow(s.dpy, w->win, rw, rh);
    XFlush(s.dpy);
  }
}

bool Display_CreateWindowFramebuffer(void *window, uint32_t *format, void **pixels, int32_t *pitch) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL || format == NULL || pixels == NULL || pitch == NULL || s.dpy == NULL) {
    return false;
  }
  if (w->image != NULL) {
    // The driver owns the buffer and resized it WITH the window (the ConfigureNotify handler reallocated it,
    // preserving the overlap): a re-call after a WINDOW_RESIZE refreshes the out-params. A metadata update,
    // not a re-allocation.
    *format = w->format;
    *pixels = w->pixels;
    *pitch = w->pitch;
    return true;
  }
  const int32_t W = w->w > 0 ? w->w : 1;
  const int32_t H = w->h > 0 ? w->h : 1;
  w->pitch = W * 4;
  // The driver's ARGB buffer (top-down: row 0 is the top edge), wrapped in an XImage header (the first
  // allocation - the resize path is the ConfigureNotify's dx_resizeFramebuffer, which keeps the overlap).
  void *newPixels = Memory_Alloc((size_t)W * (size_t)H * 4u);
  if (newPixels == NULL) {
    LogError("[display_x11] the framebuffer allocation failed (%dx%d)", W, H);
    return false;
  }
  w->pixels = newPixels;
  w->image = XCreateImage(s.dpy, s.visual, s.depth, ZPixmap, 0, (char *)w->pixels,
                          (unsigned int)W, (unsigned int)H, 32, w->pitch);
  if (w->image == NULL) {
    LogError("[display_x11] XCreateImage failed (%dx%d)", W, H);
    return false; // the pixels stay the driver's (a retry rebuilds the header)
  }
  w->format = DX_FORMAT_ARGB8888;
  *format = w->format;
  *pixels = w->pixels; // the app renders straight into the buffer (zero-copy present)
  *pitch = w->pitch;
  return true;
}

void Display_DestroyWindowFramebuffer(void *window) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL) {
    return;
  }
  if (w->image != NULL) {
    w->image->data = NULL; // this Xlib's XDestroyImage frees data; the pixels free below is the driver's
    XDestroyImage(w->image);
    w->image = NULL;
  }
  if (w->pixels != NULL) {
    Memory_Free(w->pixels);
    w->pixels = NULL;
  }
  w->pitch = 0;
  w->format = 0;
}

bool Display_UpdateWindowFramebuffer(void *window, const CruxRect *rects, int32_t count) {
  DxWindow *w = dx_findWindow(window);
  if (w == NULL || w->image == NULL || w->gc == None) {
    return false;
  }
  (void)rects; // accepted and ignored: the present is a full XPutImage
  (void)count;
  // The present covers the framebuffer's created size (a resize refreshes the record and the app recreates the
  // framebuffer).
  XPutImage(s.dpy, w->win, w->gc, w->image, 0, 0, 0, 0,
            (unsigned int)w->image->width, (unsigned int)w->image->height);
  XFlush(s.dpy);
  return true;
}

// The probe entry point (probe.c calls it after the tables resolve): the
// REAL x11 display open (XOpenDisplay + the visual-floor check). A
// successful open STAYS up - the driver's lazy connect is idempotent
// (its first CreateWindow reuses it). No feed hook here: the x11 driver
// registers its pump at first window, not at connect.
bool DlX11_Probe(void) {
  return dx_ensureDisplay();
}
