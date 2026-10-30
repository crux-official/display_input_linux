// The wayland half of the input side of display_input_linux (the input
// concern): the process-neutral event queue the display driver feeds.
//
// The queue is a 256-slot ring with no synchronization of its own: the
// stream is SINGLE-THREADED by contract - every push and pop happens on
// the app's thread (the display driver's pump runs as the feed hook,
// inside Poll/Wait/Peek). Poll/Wait/Peek drain the feed hook FIRST — the
// OS-side pump the display driver registers (a Wayland seat belongs to
// the client connection that created the window, so the display driver
// owns the connection and the normalization and pushes through this
// queue) — so a wait on an empty queue still sees events that arrive
// during it, and the hook's blocking budget IS the contract's timeout
// (a device wait, not a sleep). Without a display driver (a headless
// cell, a test harness) the hook is absent: the ring is fed by
// PushEvent alone, and a blocking wait degrades to a 1 ms poll (the
// deadline still bounds it).
//
// The ring is static (no heap); time is the platform's monotonic clock
// (Time_GetTicksNS - the time concern); diagnostics go through the
// logging concern (Log - a no-op when the cell composes no logger).

#include "kernel_types.h"
#include "logging.h"
#include "crux_time.h"
#include "input.h"
#include "input_api.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>


#define IL_RING 256
#define IL_MAX_WINDOW_LINKS 16

// --- The ring + its synchronization --------------------------------------------

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
// The signature carries the REMAINING wait budget: the pump may block up to
// `timeoutNS` on the device's fd (0 = a non-blocking drain). Events pushed
// during the pump land in the ring on this same thread, so a wait that
// blocks in the hook sees them in the ring check the moment the hook
// returns.
typedef void (*IlFeedFn)(void *ud, uint64_t timeoutNS);
static IlFeedFn s_feed = NULL;
static void *s_feed_ud = NULL;

void InWayland_SetFeedHook(void (*feed)(void *ud, uint64_t timeoutNS), void *ud) {
  s_feed = (IlFeedFn)feed;
  s_feed_ud = ud;
}

// --- The window registry (display driver -> input driver) ------------------------
// The display driver registers the OS window handle it created with the
// windowId it assigned. On windows/darwin the native event carries the
// handle and the input driver resolves the id from this table; on linux
// the display driver already knows the id when it normalizes, but it
// registers anyway (one convention across the three OS drivers).
typedef struct IlWindowLink {
  void *os_window;
  uint32_t window_id;
  bool in_use;
} IlWindowLink;
static IlWindowLink s_windows[IL_MAX_WINDOW_LINKS];

void InWayland_RegisterWindow(void *os_window, uint32_t window_id) {
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

void InWayland_UnregisterWindow(void *os_window) {
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
uint32_t InWayland_WindowIdForHandle(void *os_window) {
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

// --- The contract ----------------------------------------------------------------

// The injection point (the contract's): a test harness, or the display
// driver normalizing OS events, pushes through here. The event is
// consumed as-is (timestamp included); a full queue drops with a line to
// the log rather than block the producer.
bool InWayland_PushEvent(const CruxInputEvent *event) {
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

bool InWayland_PollEvent(CruxInputEvent *out) {
  if (s_feed != NULL) {
    s_feed(s_feed_ud, 0); // a non-blocking drain before the ring check
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

// The wait's heart: drain the feed hook (blocking up to the remaining
// budget — on the real wayland cell the hook IS the block: the display
// driver's pump waits on the connection's fd), then check the ring. An
// event pushed during the pump is already in the ring when the hook
// returns, so one round of hook-then-check is enough; the loop only
// re-checks the deadline (the hook consumes its whole budget before
// returning). Without a hook (a headless cell, a test harness) there is
// nothing to block on and the wait degrades to a 1 ms poll bounded by
// the deadline. The event is NOT popped; the caller takes it (or does
// not).
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

bool InWayland_WaitEvent(CruxInputEvent *out, uint64_t timeoutNS) {
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

// The non-consuming timed wait: blocks until an event is pending or the
// deadline passes (the feed hook's device wait, or a 1 ms poll when no
// hook is registered — the budget is honored either way); the head is
// NOT popped — the next PollEvent / WaitEvent takes it. A boot's frame
// pacing blocks here without stealing an event.
bool InWayland_PeekEvent(uint64_t timeoutNS) {
  return il_waitPending(timeoutNS);
}

