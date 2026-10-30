// The display half of display_input_linux: ONE served DisplayAPI over the
// runtime-probed backend. The probe (probe.c) picks wayland -> x11 -> none
// ONCE, at construct; every contract call dispatches on the winner. The
// none fallback mirrors display_none EXACTLY (the headless contract: no
// bit, no windows, no framebuffers - the app's Engine_Supports guards
// keep working unchanged).
//
// The backends (wayland.c / x11.c) sit on the dlopen'd tables
// (trampoline_*.h); their contract functions carry a per-backend prefix so
// this TU owns the real Display_* names.

#include "kernel_types.h"
#include "logging.h"
#include "display.h"
#include "display_api.h"
#include "probe.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// --- The backends' contract functions (renamed in their TUs) -------------------

// wayland.c
extern int32_t DlWayland_GetDisplayCount(void);
extern bool DlWayland_GetDisplayBounds(int32_t display, CruxRect *bounds);
extern void *DlWayland_CreateWindow(const CruxWindowParams *params);
extern void DlWayland_DestroyWindow(void *window);
extern void DlWayland_SetWindowTitle(void *window, const char *title);
extern void DlWayland_SetWindowPosition(void *window, int32_t x, int32_t y);
extern void DlWayland_SetWindowSize(void *window, int32_t w, int32_t h);
extern void DlWayland_SetWindowMinSize(void *window, int32_t minW, int32_t minH);
extern void DlWayland_GetWindowSizeInPixels(void *window, int32_t *w, int32_t *h);
extern void DlWayland_ShowWindow(void *window);
extern void DlWayland_HideWindow(void *window);
extern void DlWayland_RaiseWindow(void *window);
extern void DlWayland_MinimizeWindow(void *window);
extern void DlWayland_MaximizeWindow(void *window);
extern void DlWayland_RestoreWindow(void *window);
extern void DlWayland_SetWindowFullscreen(void *window, bool fullscreen);
extern bool DlWayland_CreateWindowFramebuffer(void *window, uint32_t *format, void **pixels, int32_t *pitch);
extern void DlWayland_DestroyWindowFramebuffer(void *window);
extern bool DlWayland_UpdateWindowFramebuffer(void *window, const CruxRect *rects, int32_t count);

// x11.c
extern int32_t DlX11_GetDisplayCount(void);
extern bool DlX11_GetDisplayBounds(int32_t display, CruxRect *bounds);
extern void *DlX11_CreateWindow(const CruxWindowParams *params);
extern void DlX11_DestroyWindow(void *window);
extern void DlX11_SetWindowTitle(void *window, const char *title);
extern void DlX11_SetWindowPosition(void *window, int32_t x, int32_t y);
extern void DlX11_SetWindowSize(void *window, int32_t w, int32_t h);
extern void DlX11_SetWindowMinSize(void *window, int32_t minW, int32_t minH);
extern void DlX11_GetWindowSizeInPixels(void *window, int32_t *w, int32_t *h);
extern void DlX11_ShowWindow(void *window);
extern void DlX11_HideWindow(void *window);
extern void DlX11_RaiseWindow(void *window);
extern void DlX11_MinimizeWindow(void *window);
extern void DlX11_MaximizeWindow(void *window);
extern void DlX11_RestoreWindow(void *window);
extern void DlX11_SetWindowFullscreen(void *window, bool fullscreen);
extern bool DlX11_CreateWindowFramebuffer(void *window, uint32_t *format, void **pixels, int32_t *pitch);
extern void DlX11_DestroyWindowFramebuffer(void *window);
extern bool DlX11_UpdateWindowFramebuffer(void *window, const CruxRect *rects, int32_t count);

// --- The none fallback (the display_none contract, mirrored exactly) ------------

static int32_t none_count(void) {
  return 0;
}
static bool none_bounds(int32_t display, CruxRect *bounds) {
  (void)display;
  (void)bounds;
  return false;
}
static void *none_createWindow(const CruxWindowParams *params) {
  (void)params;
  return NULL;
}
static void none_destroyWindow(void *window) {
  (void)window;
}
static void none_setWindowTitle(void *window, const char *title) {
  (void)window;
  (void)title;
}
static void none_setWindowPosition(void *window, int32_t x, int32_t y) {
  (void)window;
  (void)x;
  (void)y;
}
static void none_setWindowSize(void *window, int32_t w, int32_t h) {
  (void)window;
  (void)w;
  (void)h;
}
static void none_setWindowMinSize(void *window, int32_t minW, int32_t minH) {
  (void)window;
  (void)minW;
  (void)minH;
}
static void none_getWindowSizeInPixels(void *window, int32_t *w, int32_t *h) {
  (void)window;
  if (w != NULL) {
    *w = 0;
  }
  if (h != NULL) {
    *h = 0;
  }
}
static void none_showWindow(void *window) {
  (void)window;
}
static void none_hideWindow(void *window) {
  (void)window;
}
static void none_raiseWindow(void *window) {
  (void)window;
}
static void none_minimizeWindow(void *window) {
  (void)window;
}
static void none_maximizeWindow(void *window) {
  (void)window;
}
static void none_restoreWindow(void *window) {
  (void)window;
}
static void none_setWindowFullscreen(void *window, bool fullscreen) {
  (void)window;
  (void)fullscreen;
}
static bool none_createWindowFramebuffer(void *window, uint32_t *format, void **pixels, int32_t *pitch) {
  (void)window;
  (void)format;
  (void)pixels;
  (void)pitch;
  return false;
}
static void none_destroyWindowFramebuffer(void *window) {
  (void)window;
}
static bool none_updateWindowFramebuffer(void *window, const CruxRect *rects, int32_t count) {
  (void)window;
  (void)rects;
  (void)count;
  return false;
}

// --- The served contract (dispatch on the process-lifetime winner) ---------------

int32_t Display_GetDisplayCount(void) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: return DlWayland_GetDisplayCount();
  case CRUX_LNX_X11: return DlX11_GetDisplayCount();
  default: return none_count();
  }
}
bool Display_GetDisplayBounds(int32_t display, CruxRect *bounds) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: return DlWayland_GetDisplayBounds(display, bounds);
  case CRUX_LNX_X11: return DlX11_GetDisplayBounds(display, bounds);
  default: return none_bounds(display, bounds);
  }
}
void *Display_CreateWindow(const CruxWindowParams *params) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: return DlWayland_CreateWindow(params);
  case CRUX_LNX_X11: return DlX11_CreateWindow(params);
  default: return none_createWindow(params);
  }
}
void Display_DestroyWindow(void *window) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_DestroyWindow(window); return;
  case CRUX_LNX_X11: DlX11_DestroyWindow(window); return;
  default: none_destroyWindow(window);
  }
}
void Display_SetWindowTitle(void *window, const char *title) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_SetWindowTitle(window, title); return;
  case CRUX_LNX_X11: DlX11_SetWindowTitle(window, title); return;
  default: none_setWindowTitle(window, title);
  }
}
void Display_SetWindowPosition(void *window, int32_t x, int32_t y) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_SetWindowPosition(window, x, y); return;
  case CRUX_LNX_X11: DlX11_SetWindowPosition(window, x, y); return;
  default: none_setWindowPosition(window, x, y);
  }
}
void Display_SetWindowSize(void *window, int32_t w, int32_t h) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_SetWindowSize(window, w, h); return;
  case CRUX_LNX_X11: DlX11_SetWindowSize(window, w, h); return;
  default: none_setWindowSize(window, w, h);
  }
}
void Display_SetWindowMinSize(void *window, int32_t minW, int32_t minH) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_SetWindowMinSize(window, minW, minH); return;
  case CRUX_LNX_X11: DlX11_SetWindowMinSize(window, minW, minH); return;
  default: none_setWindowMinSize(window, minW, minH);
  }
}
void Display_GetWindowSizeInPixels(void *window, int32_t *w, int32_t *h) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_GetWindowSizeInPixels(window, w, h); return;
  case CRUX_LNX_X11: DlX11_GetWindowSizeInPixels(window, w, h); return;
  default: none_getWindowSizeInPixels(window, w, h);
  }
}
void Display_ShowWindow(void *window) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_ShowWindow(window); return;
  case CRUX_LNX_X11: DlX11_ShowWindow(window); return;
  default: none_showWindow(window);
  }
}
void Display_HideWindow(void *window) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_HideWindow(window); return;
  case CRUX_LNX_X11: DlX11_HideWindow(window); return;
  default: none_hideWindow(window);
  }
}
void Display_RaiseWindow(void *window) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_RaiseWindow(window); return;
  case CRUX_LNX_X11: DlX11_RaiseWindow(window); return;
  default: none_raiseWindow(window);
  }
}
void Display_MinimizeWindow(void *window) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_MinimizeWindow(window); return;
  case CRUX_LNX_X11: DlX11_MinimizeWindow(window); return;
  default: none_minimizeWindow(window);
  }
}
void Display_MaximizeWindow(void *window) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_MaximizeWindow(window); return;
  case CRUX_LNX_X11: DlX11_MaximizeWindow(window); return;
  default: none_maximizeWindow(window);
  }
}
void Display_RestoreWindow(void *window) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_RestoreWindow(window); return;
  case CRUX_LNX_X11: DlX11_RestoreWindow(window); return;
  default: none_restoreWindow(window);
  }
}
void Display_SetWindowFullscreen(void *window, bool fullscreen) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_SetWindowFullscreen(window, fullscreen); return;
  case CRUX_LNX_X11: DlX11_SetWindowFullscreen(window, fullscreen); return;
  default: none_setWindowFullscreen(window, fullscreen);
  }
}
bool Display_CreateWindowFramebuffer(void *window, uint32_t *format, void **pixels, int32_t *pitch) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: return DlWayland_CreateWindowFramebuffer(window, format, pixels, pitch);
  case CRUX_LNX_X11: return DlX11_CreateWindowFramebuffer(window, format, pixels, pitch);
  default: return none_createWindowFramebuffer(window, format, pixels, pitch);
  }
}
void Display_DestroyWindowFramebuffer(void *window) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: DlWayland_DestroyWindowFramebuffer(window); return;
  case CRUX_LNX_X11: DlX11_DestroyWindowFramebuffer(window); return;
  default: none_destroyWindowFramebuffer(window);
  }
}
bool Display_UpdateWindowFramebuffer(void *window, const CruxRect *rects, int32_t count) {
  switch (CruxDisplayLinux_Backend()) {
  case CRUX_LNX_WAYLAND: return DlWayland_UpdateWindowFramebuffer(window, rects, count);
  case CRUX_LNX_X11: return DlX11_UpdateWindowFramebuffer(window, rects, count);
  default: return none_updateWindowFramebuffer(window, rects, count);
  }
}

// The served struct: the display concern's contract, filled with the
// dispatch addresses (valid from load, before Construct). Extern: the
// face's Construct registers it with Engine_RegisterConcernAPI.
DisplayAPI g_display_api = {
    .version = {1, 1, 0},
    .GetDisplayCount = Display_GetDisplayCount,
    .GetDisplayBounds = Display_GetDisplayBounds,
    .CreateWindow = Display_CreateWindow,
    .DestroyWindow = Display_DestroyWindow,
    .SetWindowTitle = Display_SetWindowTitle,
    .SetWindowPosition = Display_SetWindowPosition,
    .SetWindowSize = Display_SetWindowSize,
    .SetWindowMinSize = Display_SetWindowMinSize,
    .GetWindowSizeInPixels = Display_GetWindowSizeInPixels,
    .ShowWindow = Display_ShowWindow,
    .HideWindow = Display_HideWindow,
    .RaiseWindow = Display_RaiseWindow,
    .MinimizeWindow = Display_MinimizeWindow,
    .MaximizeWindow = Display_MaximizeWindow,
    .RestoreWindow = Display_RestoreWindow,
    .SetWindowFullscreen = Display_SetWindowFullscreen,
    .CreateWindowFramebuffer = Display_CreateWindowFramebuffer,
    .DestroyWindowFramebuffer = Display_DestroyWindowFramebuffer,
    .UpdateWindowFramebuffer = Display_UpdateWindowFramebuffer,
};
