// The input half of display_input_linux: ONE served InputAPI + the
// cross-driver CruxInput_* surface over the runtime-probed backend. The
// backend is the DISPLAY half's winner (the input half "rides along" - the
// probe is owned by the display half; query it, never re-derive it):
//   - the wayland backend is the PASSIVE queue (the display half pushes
//     events in and owns the pump);
//   - the x11 backend is the active sampler (its own XOpenDisplay);
//   - the none fallback mirrors input_none EXACTLY (no bit; the bounded
//     waits honor the timeout; the app's Engine_Supports guards hold).

#include "kernel_types.h"
#include "logging.h"
#include "crux_time.h"
#include "input.h"
#include "input_api.h"
#include "probe.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

// --- The backends' contract + cross-driver functions (renamed in their TUs) ----

// queue.c (the passive wayland queue)
extern bool InWayland_PollEvent(CruxInputEvent *out);
extern bool InWayland_WaitEvent(CruxInputEvent *out, uint64_t timeoutNS);
extern bool InWayland_PeekEvent(uint64_t timeoutNS);
extern bool InWayland_PushEvent(const CruxInputEvent *event);
extern void InWayland_SetFeedHook(void (*feed)(void *ud, uint64_t timeoutNS), void *ud);
extern void InWayland_RegisterWindow(void *os_window, uint32_t window_id);
extern void InWayland_UnregisterWindow(void *os_window);
extern uint32_t InWayland_WindowIdForHandle(void *os_window);

// x11_input.c (the active x11 sampler)
extern bool InX11_PollEvent(CruxInputEvent *out);
extern bool InX11_WaitEvent(CruxInputEvent *out, uint64_t timeoutNS);
extern bool InX11_PeekEvent(uint64_t timeoutNS);
extern bool InX11_PushEvent(const CruxInputEvent *event);
extern void InX11_SetFeedHook(void (*feed)(void *ud, uint64_t timeoutNS), void *ud);
extern void InX11_RegisterWindow(void *os_window, uint32_t window_id);
extern void InX11_UnregisterWindow(void *os_window);
extern uint32_t InX11_WindowIdForHandle(void *os_window);
extern void InX11_ForwardX11Event(void *os_window, uint32_t kind, int32_t x, int32_t y, uint32_t code, uint32_t button, bool down, int32_t wheelX, int32_t wheelY, bool repeat);

// --- The none fallback (the input_none contract, mirrored exactly) ----------------

static uint64_t in_nowNS(void) {
  return Time_GetTicksNS(); // the platform's monotonic clock (the time concern)
}

// The bounded wait: sleep to the deadline in 1 ms steps (nothing to
// interrupt on - the granularity is the cost).
static void in_waitNS(uint64_t timeoutNS) {
  if (timeoutNS == 0) {
    return;
  }
  const uint64_t deadline = in_nowNS() + timeoutNS;
  for (;;) {
    const uint64_t now = in_nowNS();
    if (now >= deadline) {
      return;
    }
    uint64_t step = deadline - now;
    if (step > 1000000ULL) {
      step = 1000000ULL;
    }
#ifdef _WIN32
    Sleep((DWORD)(step / 1000000ULL));
#else
    const struct timespec ts = { (time_t)(step / 1000000000ULL), (long)(step % 1000000000ULL) };
    nanosleep(&ts, NULL);
#endif
  }
}

static bool none_pollEvent(CruxInputEvent *out) {
  (void)out;
  return false;
}
static bool none_waitEvent(CruxInputEvent *out, uint64_t timeoutNS) {
  (void)out;
  in_waitNS(timeoutNS);
  return false;
}
static bool none_peekEvent(uint64_t timeoutNS) {
  in_waitNS(timeoutNS);
  return false;
}
static bool none_pushEvent(const CruxInputEvent *event) {
  (void)event;
  return false;
}

// --- The served contract (dispatch on the display half's winner) ------------------

bool Input_PollEvent(CruxInputEvent *out) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: return InWayland_PollEvent(out);
  case CRUX_LNX_X11: return InX11_PollEvent(out);
  default: return none_pollEvent(out);
  }
}
bool Input_WaitEvent(CruxInputEvent *out, uint64_t timeoutNS) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: return InWayland_WaitEvent(out, timeoutNS);
  case CRUX_LNX_X11: return InX11_WaitEvent(out, timeoutNS);
  default: return none_waitEvent(out, timeoutNS);
  }
}
bool Input_PeekEvent(uint64_t timeoutNS) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: return InWayland_PeekEvent(timeoutNS);
  case CRUX_LNX_X11: return InX11_PeekEvent(timeoutNS);
  default: return none_peekEvent(timeoutNS);
  }
}
bool Input_PushEvent(const CruxInputEvent *event) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: return InWayland_PushEvent(event);
  case CRUX_LNX_X11: return InX11_PushEvent(event);
  default: return none_pushEvent(event);
  }
}

// --- The cross-driver surface (the display halves call these; the house
//     pattern: plain externs, defined ONCE in this TU) -------------------------------

void CruxInput_SetFeedHook(void (*feed)(void *ud, uint64_t timeoutNS), void *ud) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: InWayland_SetFeedHook(feed, ud); return;
  case CRUX_LNX_X11: InX11_SetFeedHook(feed, ud); return;
  default: (void)feed; (void)ud;
  }
}
void CruxInput_RegisterWindow(void *os_window, uint32_t window_id) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: InWayland_RegisterWindow(os_window, window_id); return;
  case CRUX_LNX_X11: InX11_RegisterWindow(os_window, window_id); return;
  default: (void)os_window; (void)window_id;
  }
}
void CruxInput_UnregisterWindow(void *os_window) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: InWayland_UnregisterWindow(os_window); return;
  case CRUX_LNX_X11: InX11_UnregisterWindow(os_window); return;
  default: (void)os_window;
  }
}
uint32_t CruxInput_WindowIdForHandle(void *os_window) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: return InWayland_WindowIdForHandle(os_window);
  case CRUX_LNX_X11: return InX11_WindowIdForHandle(os_window);
  default: (void)os_window; return 0;
  }
}
void CruxInput_ForwardX11Event(void *os_window, uint32_t kind, int32_t x, int32_t y, uint32_t code, uint32_t button, bool down, int32_t wheelX, int32_t wheelY, bool repeat) {
  // The x11 display half's forwarder - only live on the x11 backend.
  if (CruxDisplayLinux_Backend() == CRUX_LNX_X11) {
    InX11_ForwardX11Event(os_window, kind, x, y, code, button, down, wheelX, wheelY, repeat);
  }
}

// The served struct: the input concern's contract, filled with the
// dispatch addresses (valid from load, before Construct). Extern: the
// face's Construct registers it with Engine_RegisterConcernAPI.
InputAPI g_input_api = {
    .version = {1, 1, 0},
    .PollEvent = Input_PollEvent,
    .WaitEvent = Input_WaitEvent,
    .PeekEvent = Input_PeekEvent,
    .PushEvent = Input_PushEvent,
};
