// The X11 input driver: the process-neutral event queue the display driver feeds. The queue is a 256-slot ring
// with no synchronization of its own - the stream is SINGLE-THREADED by contract (every push and pop is the
// app's thread: the display driver's pump runs as the feed hook, inside Poll/Wait/Peek). Poll/Wait/Peek drain
// the feed hook FIRST - the OS-side pump the display driver registers, whose blocking budget is the contract's
// timeout (a device wait, not a sleep). Without a display driver (a headless cell, a test harness) the hook is
// absent, the ring is fed by PushEvent alone, and a blocking wait degrades to a 1 ms poll (the deadline still
// bounds it).
//
// Timestamps are the pump time: the X event's `time` field is server wall-clock ms, not a monotonic stamp. Key
// repeat is synthesized by the display driver (Xlib delivers no repeat flag); this driver records it as-is.
//
// The mod bits are sampled at forward time on the driver's OWN X connection (the second in the process - the
// display driver's is the event source, this one is the sampler): XQueryKeymap for the shift/ctrl/alt pairs,
// the Xkb named indicators for caps/num. GUI and SCROLL are never set: the X11 core has no reliable super-key
// state and no scroll-lock keycode (a v1 gap of this stack's API). A failed open or an absent Xkb extension
// degrades the affected bits to 0 for the process life (one log line; no reconnect). The sampled state is
// at forward time, not event time (a microsecond race).
//
// The ring is static (no heap); time is the platform's monotonic clock
// (Time_GetTicksNS - the time concern); diagnostics go through the
// logging concern (Log - a no-op when the cell composes no logger).

#include <X11/Xlib.h>
#include "kernel_types.h"
#include "logging.h"
#include "input.h"
#include "input_api.h"
#include "crux_time.h"
#include <X11/XKBlib.h>
#include <X11/keysym.h>

// The type headers first (their real prototypes are dead - nothing links
// libX11), then the rename layer (every call site below hits the
// dlopen'd table).
#include "trampoline_x11.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>


// --- The forwarder contract's constants -----------------------------------------

// The forwarder's kind values (the display driver carries the identical set as DX_EVT_*; keep them in sync):
#define IX_EVT_MOTION 0 // x,y window coords, y-down
#define IX_EVT_BUTTON 1 // button 1-based, down
#define IX_EVT_WHEEL 2  // wheelX/wheelY notches
#define IX_EVT_KEY 3    // code = the X11 keycode, down, repeat
#define IX_EVT_FOCUS 4  // down = focused
#define IX_EVT_RESIZE 5 // x,y = new client w,h
#define IX_EVT_CLOSE 6


// --- The ring + its synchronization --------------------------------------------

#define IL_RING 256
#define IL_MAX_WINDOW_LINKS 16

static CruxInputEvent s_ring[IL_RING];
static int s_head = 0; // next write
static int s_tail = 0; // next read


static uint64_t il_nowNS(void) {
  return Time_GetTicksNS(); // the platform's monotonic clock (the time concern)
}

static void il_deadlineNS(uint64_t timeoutNS, uint64_t *out) {
  *out = timeoutNS == 0 ? 0 : il_nowNS() + timeoutNS;
}

// --- The feed hook (the OS-side pump the display driver registers) --------------
// The argument is the REMAINING wait budget: the pump may block up to it on the device (0 = a non-blocking
// drain). Events pushed during the pump land in the ring on this same thread, so a wait that blocks in the
// hook sees them in the ring check the moment the hook returns.
typedef void (*IlFeedFn)(void *ud, uint64_t timeoutNS);
static IlFeedFn s_feed = NULL;
static void *s_feed_ud = NULL;

void InX11_SetFeedHook(void (*feed)(void *ud, uint64_t timeoutNS), void *ud) {
  s_feed = (IlFeedFn)feed;
  s_feed_ud = ud;
}

// --- The window registry (display driver -> input driver) ------------------------
// The display driver registers the OS handle it created with the windowId it assigned; the forwarder resolves
// the id from here (one convention across the OS drivers). The entry also carries the window's last known
// pointer position - the simplest home for the forwarder's per-window tracking (no second table, no second
// lock); MOTION deltas and BUTTON positions derive from it, and both refresh it.
typedef struct IlWindowLink {
  void *os_window;
  uint32_t window_id;
  bool in_use;
  int32_t last_x; // the last known pointer position (client coords, y-down)
  int32_t last_y;
  bool have_pos;  // the position is a real event, not the zero init
} IlWindowLink;
static IlWindowLink s_windows[IL_MAX_WINDOW_LINKS];

void InX11_RegisterWindow(void *os_window, uint32_t window_id) {
  if (os_window == NULL) {
    return;
  }
  for (int i = 0; i < IL_MAX_WINDOW_LINKS; i++) {
    if (s_windows[i].os_window == os_window) {
      s_windows[i].window_id = window_id; // re-register: the id moves
      return;
    }
  }
  for (int i = 0; i < IL_MAX_WINDOW_LINKS; i++) {
    if (!s_windows[i].in_use) {
      s_windows[i].os_window = os_window;
      s_windows[i].window_id = window_id;
      s_windows[i].in_use = true;
      break;
    }
  }
}

void InX11_UnregisterWindow(void *os_window) {
  if (os_window == NULL) {
    return;
  }
  for (int i = 0; i < IL_MAX_WINDOW_LINKS; i++) {
    if (s_windows[i].in_use && s_windows[i].os_window == os_window) {
      s_windows[i].in_use = false;
      s_windows[i].os_window = NULL;
      break;
    }
  }
}

// 0 when the handle is not registered (the neutral "no window" id).
uint32_t InX11_WindowIdForHandle(void *os_window) {
  if (os_window == NULL) {
    return 0;
  }
  uint32_t id = 0;
  for (int i = 0; i < IL_MAX_WINDOW_LINKS; i++) {
    if (s_windows[i].in_use && s_windows[i].os_window == os_window) {
      id = s_windows[i].window_id;
      break;
    }
  }
  return id;
}

// --- The scancode table (the X server's XFree86 keycode -> the stable cross-OS scancode) ---
// The values are the stable cross-OS set (the kernel's input note carries the reference): the same physical key
// carries the same scancode on every platform. 0 is "unknown" (the table's value for unmapped keys). The
// table's index is the Linux evdev code (A at 30, KP_ENTER at 96); the X server's XFree86 keycodes run from 8
// (Escape = 9, 'a' = 38), so an X11 keycode indexes at keycode - 8.
// Source: the SDL3 3.4.18 xfree86 table (MIT-licensed, https://github.com/libsdl-org/SDL).
static const uint16_t il_xfree86_scancodes[248] = {
    0, 41, 30, 31, 32, 33, 34, 35,
    36, 37, 38, 39, 45, 46, 42, 43,
    20, 26, 8, 21, 23, 28, 24, 12,
    18, 19, 47, 48, 40, 224, 4, 22,
    7, 9, 10, 11, 13, 14, 15, 51,
    52, 53, 225, 49, 29, 27, 6, 25,
    5, 17, 16, 54, 55, 56, 229, 85,
    226, 44, 57, 58, 59, 60, 61, 62,
    63, 64, 65, 66, 67, 83, 71, 95,
    96, 97, 86, 92, 93, 94, 87, 89,
    90, 91, 98, 99, 230, 257, 100, 68,
    69, 135, 146, 147, 138, 136, 139, 0,
    88, 228, 84, 70, 230, 0, 74, 82,
    75, 80, 79, 77, 81, 78, 73, 76,
    0, 127, 129, 128, 102, 103, 215, 72,
    0, 99, 144, 145, 137, 227, 231, 101,
    155, 121, 0, 122, 165, 124, 0, 125,
    126, 123, 117, 118, 0, 0, 258, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 286, 0, 282, 283,
    0, 270, 270, 267, 271, 268, 269, 264,
    266, 0, 0, 104, 281, 285, 0, 0,
    0, 0, 0, 182, 183, 273, 121, 104,
    105, 106, 107, 108, 109, 110, 111, 112,
    113, 114, 115, 257, 0, 0, 0, 0,
    262, 263, 0, 0, 0, 0, 275, 262,
    265, 0, 70, 0, 0, 0, 0, 0,
    0, 280, 0, 0, 0, 0, 0, 155,
    0, 0, 272, 0, 0, 0, 0, 0,
    0, 0, 277, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};

static uint32_t il_keycodeToScancode(uint32_t keycode) {
  if (keycode < 9 || keycode > 255) {
    return 0;
  }
  return il_xfree86_scancodes[keycode - 8];
}

// --- The modifier state (the driver's own X connection) ---------------------------
static Display *s_xdpy = NULL;
static int s_xconnect_tried = 0;
static int s_xkb_present = 0;
static KeyCode s_shift_l, s_shift_r;
static KeyCode s_ctrl_l, s_ctrl_r;
static KeyCode s_alt_l, s_alt_r;
static Atom s_caps_atom, s_num_atom;

static void il_x11Connect(void) {
  if (s_xdpy != NULL || s_xconnect_tried) {
    return;
  }
  s_xconnect_tried = 1;
  s_xdpy = XOpenDisplay(NULL);
  if (s_xdpy == NULL) {
    LogWarning("[input_x11] sampler XOpenDisplay failed; mods degrade to 0");
    return;
  }
  // The modifier keycodes, cached once (a keysym absent from the keymap comes back 0: that key never sets its bit).
  s_shift_l = XKeysymToKeycode(s_xdpy, XK_Shift_L);
  s_shift_r = XKeysymToKeycode(s_xdpy, XK_Shift_R);
  s_ctrl_l = XKeysymToKeycode(s_xdpy, XK_Control_L);
  s_ctrl_r = XKeysymToKeycode(s_xdpy, XK_Control_R);
  s_alt_l = XKeysymToKeycode(s_xdpy, XK_Alt_L);
  s_alt_r = XKeysymToKeycode(s_xdpy, XK_Alt_R);
  s_caps_atom = XInternAtom(s_xdpy, "Caps Lock", False);
  s_num_atom = XInternAtom(s_xdpy, "Num Lock", False);
  int major = 0, minor = 0;
  if (XkbUseExtension(s_xdpy, &major, &minor)) {
    s_xkb_present = 1;
  } else {
    LogWarning("[input_x11] Xkb extension absent; caps/num degrade to 0");
  }
}

// True when the keycode is in the XQueryKeymap down-vector (keycode k's state is bit (k-1)%8 of byte (k-1)/8).
static bool il_kcDown(const char *keys, KeyCode kc) {
  return kc != 0 && (keys)[((kc - 1) >> 3)] & (1 << ((kc - 1) & 7));
}

// The current mod bits (the kernel's CRUX_MOD_*), sampled on the driver's own connection at forward time: either
// key of the L/R pair down sets the pair's bits (the same logical state carries the same bits on every OS).
static uint32_t il_mods(void) {
  uint32_t m = 0;
  il_x11Connect();
  if (s_xdpy == NULL) {
    return 0;
  }
  char keys[32];
  if (XQueryKeymap(s_xdpy, keys) == 0) {
    return 0; // a sampler hiccup; the event still flows modless
  }
  if (il_kcDown(keys, s_shift_l) || il_kcDown(keys, s_shift_r)) {
    m |= CRUX_MOD_SHIFT;
  }
  if (il_kcDown(keys, s_ctrl_l) || il_kcDown(keys, s_ctrl_r)) {
    m |= CRUX_MOD_CTRL;
  }
  if (il_kcDown(keys, s_alt_l) || il_kcDown(keys, s_alt_r)) {
    m |= CRUX_MOD_ALT;
  }
  if (s_xkb_present) {
    Bool on = False;
    if (XkbGetNamedIndicator(s_xdpy, s_caps_atom, NULL, &on, NULL, NULL) && on) {
      m |= CRUX_MOD_CAPS;
    }
    on = False;
    if (XkbGetNamedIndicator(s_xdpy, s_num_atom, NULL, &on, NULL, NULL) && on) {
      m |= CRUX_MOD_NUM;
    }
  }
  return m;
}

// --- The contract ----------------------------------------------------------------

// The injection point: the event is consumed as-is (timestamp included); a full queue drops with a log line
// rather than block the producer.
bool InX11_PushEvent(const CruxInputEvent *event) {
  if (event == NULL) {
    return false;
  }
  const int head = s_head;
  const int next = (head + 1) % IL_RING;
  if (next == s_tail) {
    LogError("[display_input_linux] event queue full, dropping %d", (int)event->kind);
    return false;
  }
  s_ring[head] = *event;
  s_head = next;
  return true;
}

bool InX11_PollEvent(CruxInputEvent *out) {
  if (s_feed != NULL) {
    s_feed(s_feed_ud, 0);
  }
  if (s_tail == s_head) {
    return false;
  }
  CruxInputEvent ev = s_ring[s_tail];
  s_tail = (s_tail + 1) % IL_RING;
  if (out != NULL) {
    *out = ev;
  }
  return true;
}

// The wait's heart: drain the feed hook (blocking up to the remaining budget - on the real X11 cell the hook IS
// the block: the display driver's pump waits on the X connection), then check the ring. An event pushed during
// the pump is already in the ring when the hook returns, so one round of hook-then-check is enough; the loop
// only re-checks the deadline (the hook consumes its whole budget before returning). Without a hook (a headless
// cell, a test harness) there is nothing to block on and the wait degrades to a 1 ms poll bounded by the
// deadline.
static bool il_waitPending(uint64_t timeoutNS) {
  uint64_t deadline = 0;
  il_deadlineNS(timeoutNS, &deadline);
  for (;;) {
    uint64_t now = il_nowNS();
    uint64_t remaining = 0;
    if (deadline != 0 && now < deadline) {
      remaining = deadline - now;
    }
    if (s_feed != NULL) {
      s_feed(s_feed_ud, remaining); // may block on the device up to `remaining`
    }
    if (s_tail != s_head) {
      return true;
    }
    if (timeoutNS == 0) {
      return false;
    }
    now = il_nowNS();
    if (deadline != 0 && now >= deadline) {
      return false;
    }
    usleep(1000); // no device to block on: a 1 ms poll (the deadline bounds it)
  }
}

bool InX11_WaitEvent(CruxInputEvent *out, uint64_t timeoutNS) {
  if (!il_waitPending(timeoutNS)) {
    return false;
  }
  CruxInputEvent ev = s_ring[s_tail];
  s_tail = (s_tail + 1) % IL_RING;
  if (out != NULL) {
    *out = ev;
  }
  return true;
}

// The non-consuming timed wait: the head is not popped (the next PollEvent / WaitEvent takes it).
bool InX11_PeekEvent(uint64_t timeoutNS) {
  return il_waitPending(timeoutNS);
}

// --- The forwarder (the display driver's raw facts -> the neutral event) ---------
// The display driver's event handler calls this for each input-relevant X event. The native event carries the
// Window; the handle resolves through the registry (an unregistered window - mid-destruction - drops the event
// with a log line). Single-threaded by contract, so the resolve and the per-window position read/update need no
// synchronization; the KEY's mods are sampled first (the XQueryKeymap / XkbGetNamedIndicator calls are server
// round-trips on the driver's own connection).
//
// Pointer position (the registry entry's tracker): a MOTION reports the event's x,y plus the delta from the last
// tracked one (zero on the window's first event) and refreshes it. A BUTTON reports the last known position
// (the button's own when the window has seen no motion yet) and refreshes it to the button's coordinates (a click
// is a confirmed position). A WHEEL reports its own coordinates and leaves the tracker alone (a wheel does not
// move the pointer). Unmapped keycodes are noted to the log once per code for the process life (a held key would
// otherwise line the log every repeat).
static bool s_unmappedNoted[256];

void InX11_ForwardX11Event(void *os_window, uint32_t kind, int32_t x, int32_t y,
                                uint32_t code, uint32_t button, bool down,
                                int32_t wheelX, int32_t wheelY, bool repeat) {
  uint32_t mods = 0;
  if (kind == IX_EVT_KEY) {
    mods = il_mods();
  }
  IlWindowLink *link = NULL;
  for (int i = 0; i < IL_MAX_WINDOW_LINKS; i++) {
    if (s_windows[i].in_use && s_windows[i].os_window == os_window) {
      link = &s_windows[i];
      break;
    }
  }
  if (link == NULL) {
    LogError("[input_x11] event for unregistered window %p, dropping", os_window);
    return;
  }

  CruxInputEvent ev = {0};
  ev.timestampNS = il_nowNS(); // the pump time (the X event carries wall-clock ms)
  switch (kind) {
    case IX_EVT_MOTION: {
      ev.kind = CRUX_INPUT_MOUSE_MOTION;
      ev.data.mouse.windowId = link->window_id;
      ev.data.mouse.x = x;
      ev.data.mouse.y = y;
      ev.data.mouse.dx = link->have_pos ? x - link->last_x : 0;
      ev.data.mouse.dy = link->have_pos ? y - link->last_y : 0;
      link->last_x = x;
      link->last_y = y;
      link->have_pos = true;
      break;
    }
    case IX_EVT_BUTTON: {
      ev.kind = CRUX_INPUT_MOUSE_BUTTON;
      ev.data.mouse.windowId = link->window_id;
      if (link->have_pos) {
        ev.data.mouse.x = link->last_x;
        ev.data.mouse.y = link->last_y;
      } else {
        ev.data.mouse.x = x;
        ev.data.mouse.y = y;
      }
      link->last_x = x;
      link->last_y = y;
      link->have_pos = true;
      ev.data.mouse.button = button;
      ev.data.mouse.down = down;
      break;
    }
    case IX_EVT_WHEEL: {
      ev.kind = CRUX_INPUT_MOUSE_WHEEL;
      ev.data.mouse.windowId = link->window_id;
      ev.data.mouse.x = x;
      ev.data.mouse.y = y;
      ev.data.mouse.wheelX = wheelX;
      ev.data.mouse.wheelY = wheelY;
      break;
    }
    case IX_EVT_KEY: {
      ev.kind = down ? CRUX_INPUT_KEY_DOWN : CRUX_INPUT_KEY_UP;
      ev.data.key.windowId = link->window_id;
      ev.data.key.scancode = il_keycodeToScancode(code);
      if (ev.data.key.scancode == 0 && code < 256 && !s_unmappedNoted[code]) {
        s_unmappedNoted[code] = true;
        LogWarning("[input_x11] unmapped keycode %u, sending scancode 0 (unknown)",
                            (unsigned)code);
      }
      ev.data.key.mods = mods;
      ev.data.key.repeat = repeat;
      break;
    }
    case IX_EVT_FOCUS: {
      ev.kind = CRUX_INPUT_WINDOW_FOCUS;
      ev.data.win.windowId = link->window_id;
      ev.data.win.focused = down;
      break;
    }
    case IX_EVT_RESIZE: {
      ev.kind = CRUX_INPUT_WINDOW_RESIZE;
      ev.data.win.windowId = link->window_id;
      ev.data.win.w = x;
      ev.data.win.h = y;
      break;
    }
    case IX_EVT_CLOSE: {
      ev.kind = CRUX_INPUT_WINDOW_CLOSE;
      ev.data.win.windowId = link->window_id;
      break;
    }
    default:
      LogError("[input_x11] unknown forward kind %u, dropping", (unsigned)kind);
      return;
  }
  InX11_PushEvent(&ev);
}

