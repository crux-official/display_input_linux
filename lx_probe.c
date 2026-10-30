// The dlopen machinery of display_input_linux: the per-library symbol tables
// (g_wl / g_xkb / g_x11), the X-macro symbol lists, the trampoline rename
// layer, and the generic dlopen/dlsym loaders. It owns NO probe SEMANTICS:
// the probe order, the env knob, the winner state, and the
// tentative/rollback live in probe.c (the display half is where the
// backend choice belongs; the input half rides along by querying it).
//
// The soname candidate lists and the REQUIRED/OPTIONAL split inside the
// symbol lists are DATA (one candidate per library; wl_proxy_marshal_flags
// is the one version-gated symbol, REQUIRED).
//
// POSIX-only: the machinery dlopens, and the package declares
// supported_platforms = ["linux"] - it reaches only linux cells (a bundle
// is one reference in concern_implementations; no addendum needed).

#define _GNU_SOURCE
// The backend type headers FIRST (the table prototypes name their types -
// see the wl_tables.h / x11_tables.h include-order contracts). The real
// prototypes are dead: nothing links the windowing libraries - every call
// goes through the tables (the trampoline rename layer in the driver TUs).
#include <wayland-client-core.h>
#include <xkbcommon/xkbcommon.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

#include "lx_probe.h"
#include "wl_tables.h"
#include "x11_tables.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

// --- The resolved tables + handles ----------------------------------------------

WlTable g_wl;
XkbTable g_xkb;
X11Table g_x11;

static void *s_wl_lib = NULL;
static void *s_xkb_lib = NULL;
static void *s_x11_lib = NULL;

// --- The stable-protocol interface tables (binary-local) --------------------------
//
// The retargeted backends' protocol inlines (the system's
// wayland-client-protocol.h + the embedded xdg block) take the core
// interfaces' DATA symbols' addresses by name; nothing links
// libwayland-client, so those extern references bind HERE (the dlopen'd
// library's own copies still gate the probe - the WL_IFACE set in
// wl_syms.h). Dumped from this box's libwayland-client.so.0 by
// tools/dump_wl_ifaces.c (the authoring dumper); rerunning it twice
// must leave this region unchanged.

// >>> wl-stable-ifaces:BEGIN (generated - do not edit; the dumper is tools/dump_wl_ifaces.c)
extern const struct wl_interface wl_buffer_interface;
extern const struct wl_interface wl_callback_interface;
extern const struct wl_interface wl_compositor_interface;
extern const struct wl_interface wl_keyboard_interface;
extern const struct wl_interface wl_output_interface;
extern const struct wl_interface wl_pointer_interface;
extern const struct wl_interface wl_region_interface;
extern const struct wl_interface wl_registry_interface;
extern const struct wl_interface wl_seat_interface;
extern const struct wl_interface wl_shm_interface;
extern const struct wl_interface wl_shm_pool_interface;
extern const struct wl_interface wl_surface_interface;
extern const struct wl_interface wl_touch_interface;
static const struct wl_interface *wl_buffer_types[] = {
	NULL,
};
static const struct wl_message wl_buffer_requests[] = {
	{ "destroy", "", wl_buffer_types + 0 },
};
static const struct wl_message wl_buffer_events[] = {
	{ "release", "", wl_buffer_types + 0 },
};
const struct wl_interface wl_buffer_interface = {
	"wl_buffer", 1,
	1, wl_buffer_requests,
	1, wl_buffer_events
};

static const struct wl_interface *wl_callback_types[] = {
	NULL,
};
static const struct wl_message wl_callback_events[] = {
	{ "done", "u", wl_callback_types + 0 },
};
const struct wl_interface wl_callback_interface = {
	"wl_callback", 1,
	0, NULL,
	1, wl_callback_events
};

static const struct wl_interface *wl_compositor_types[] = {
	&wl_surface_interface,
	&wl_region_interface,
};
static const struct wl_message wl_compositor_requests[] = {
	{ "create_surface", "n", wl_compositor_types + 0 },
	{ "create_region", "n", wl_compositor_types + 1 },
};
const struct wl_interface wl_compositor_interface = {
	"wl_compositor", 6,
	2, wl_compositor_requests,
	0, NULL
};

static const struct wl_interface *wl_keyboard_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	&wl_surface_interface,
	NULL,
	NULL,
	&wl_surface_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};
static const struct wl_message wl_keyboard_requests[] = {
	{ "release", "3", wl_keyboard_types + 0 },
};
static const struct wl_message wl_keyboard_events[] = {
	{ "keymap", "uhu", wl_keyboard_types + 1 },
	{ "enter", "uoa", wl_keyboard_types + 4 },
	{ "leave", "uo", wl_keyboard_types + 7 },
	{ "key", "uuuu", wl_keyboard_types + 9 },
	{ "modifiers", "uuuuu", wl_keyboard_types + 13 },
	{ "repeat_info", "4ii", wl_keyboard_types + 18 },
};
const struct wl_interface wl_keyboard_interface = {
	"wl_keyboard", 10,
	1, wl_keyboard_requests,
	6, wl_keyboard_events
};

static const struct wl_interface *wl_output_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};
static const struct wl_message wl_output_requests[] = {
	{ "release", "3", wl_output_types + 0 },
};
static const struct wl_message wl_output_events[] = {
	{ "geometry", "iiiiissi", wl_output_types + 1 },
	{ "mode", "uiii", wl_output_types + 9 },
	{ "done", "2", wl_output_types + 13 },
	{ "scale", "2i", wl_output_types + 14 },
	{ "name", "4s", wl_output_types + 16 },
	{ "description", "4s", wl_output_types + 18 },
};
const struct wl_interface wl_output_interface = {
	"wl_output", 4,
	1, wl_output_requests,
	6, wl_output_events
};

static const struct wl_interface *wl_pointer_types[] = {
	NULL,
	&wl_surface_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	&wl_surface_interface,
	NULL,
	NULL,
	NULL,
	&wl_surface_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};
static const struct wl_message wl_pointer_requests[] = {
	{ "set_cursor", "u?oii", wl_pointer_types + 0 },
	{ "release", "3", wl_pointer_types + 5 },
};
static const struct wl_message wl_pointer_events[] = {
	{ "enter", "uoff", wl_pointer_types + 6 },
	{ "leave", "uo", wl_pointer_types + 10 },
	{ "motion", "uff", wl_pointer_types + 12 },
	{ "button", "uuuu", wl_pointer_types + 15 },
	{ "axis", "uuf", wl_pointer_types + 19 },
	{ "frame", "5", wl_pointer_types + 22 },
	{ "axis_source", "5u", wl_pointer_types + 23 },
	{ "axis_stop", "5uu", wl_pointer_types + 25 },
	{ "axis_discrete", "5ui", wl_pointer_types + 28 },
	{ "axis_value120", "8ui", wl_pointer_types + 31 },
	{ "axis_relative_direction", "9uu", wl_pointer_types + 34 },
};
const struct wl_interface wl_pointer_interface = {
	"wl_pointer", 10,
	2, wl_pointer_requests,
	11, wl_pointer_events
};

static const struct wl_interface *wl_region_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};
static const struct wl_message wl_region_requests[] = {
	{ "destroy", "", wl_region_types + 0 },
	{ "add", "iiii", wl_region_types + 0 },
	{ "subtract", "iiii", wl_region_types + 4 },
};
const struct wl_interface wl_region_interface = {
	"wl_region", 1,
	3, wl_region_requests,
	0, NULL
};

static const struct wl_interface *wl_registry_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};
static const struct wl_message wl_registry_requests[] = {
	{ "bind", "usun", wl_registry_types + 0 },
};
static const struct wl_message wl_registry_events[] = {
	{ "global", "usu", wl_registry_types + 4 },
	{ "global_remove", "u", wl_registry_types + 7 },
};
const struct wl_interface wl_registry_interface = {
	"wl_registry", 1,
	1, wl_registry_requests,
	2, wl_registry_events
};

static const struct wl_interface *wl_seat_types[] = {
	&wl_pointer_interface,
	&wl_keyboard_interface,
	&wl_touch_interface,
	NULL,
	NULL,
	NULL,
	NULL,
};
static const struct wl_message wl_seat_requests[] = {
	{ "get_pointer", "n", wl_seat_types + 0 },
	{ "get_keyboard", "n", wl_seat_types + 1 },
	{ "get_touch", "n", wl_seat_types + 2 },
	{ "release", "5", wl_seat_types + 3 },
};
static const struct wl_message wl_seat_events[] = {
	{ "capabilities", "u", wl_seat_types + 4 },
	{ "name", "2s", wl_seat_types + 5 },
};
const struct wl_interface wl_seat_interface = {
	"wl_seat", 10,
	4, wl_seat_requests,
	2, wl_seat_events
};

static const struct wl_interface *wl_shm_types[] = {
	&wl_shm_pool_interface,
	NULL,
	NULL,
	NULL,
	NULL,
};
static const struct wl_message wl_shm_requests[] = {
	{ "create_pool", "nhi", wl_shm_types + 0 },
	{ "release", "2", wl_shm_types + 3 },
};
static const struct wl_message wl_shm_events[] = {
	{ "format", "u", wl_shm_types + 4 },
};
const struct wl_interface wl_shm_interface = {
	"wl_shm", 2,
	2, wl_shm_requests,
	1, wl_shm_events
};

static const struct wl_interface *wl_shm_pool_types[] = {
	&wl_buffer_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};
static const struct wl_message wl_shm_pool_requests[] = {
	{ "create_buffer", "niiiiu", wl_shm_pool_types + 0 },
	{ "destroy", "", wl_shm_pool_types + 6 },
	{ "resize", "i", wl_shm_pool_types + 6 },
};
const struct wl_interface wl_shm_pool_interface = {
	"wl_shm_pool", 2,
	3, wl_shm_pool_requests,
	0, NULL
};

static const struct wl_interface *wl_surface_types[] = {
	&wl_buffer_interface,
	NULL,
	NULL,
	&wl_callback_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	&wl_callback_interface,
	&wl_region_interface,
	&wl_region_interface,
	&wl_region_interface,
	&wl_output_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	&wl_output_interface,
	&wl_output_interface,
	NULL,
	NULL,
	NULL,
	NULL,
};
static const struct wl_message wl_surface_requests[] = {
	{ "destroy", "", wl_surface_types + 0 },
	{ "attach", "?oii", wl_surface_types + 0 },
	{ "damage", "iiii", wl_surface_types + 4 },
	{ "frame", "n", wl_surface_types + 8 },
	{ "set_opaque_region", "?o", wl_surface_types + 9 },
	{ "set_input_region", "?o", wl_surface_types + 11 },
	{ "commit", "", wl_surface_types + 13 },
	{ "set_buffer_transform", "2i", wl_surface_types + 13 },
	{ "set_buffer_scale", "3i", wl_surface_types + 15 },
	{ "damage_buffer", "4iiii", wl_surface_types + 17 },
	{ "offset", "5ii", wl_surface_types + 22 },
};
static const struct wl_message wl_surface_events[] = {
	{ "enter", "o", wl_surface_types + 25 },
	{ "leave", "o", wl_surface_types + 26 },
	{ "preferred_buffer_scale", "6i", wl_surface_types + 27 },
	{ "preferred_buffer_transform", "6u", wl_surface_types + 29 },
};
const struct wl_interface wl_surface_interface = {
	"wl_surface", 6,
	11, wl_surface_requests,
	4, wl_surface_events
};

static const struct wl_interface *wl_touch_types[] = {
	NULL,
	NULL,
	NULL,
	&wl_surface_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};
static const struct wl_message wl_touch_requests[] = {
	{ "release", "3", wl_touch_types + 0 },
};
static const struct wl_message wl_touch_events[] = {
	{ "down", "uuoiff", wl_touch_types + 1 },
	{ "up", "uui", wl_touch_types + 7 },
	{ "motion", "uiff", wl_touch_types + 10 },
	{ "frame", "", wl_touch_types + 14 },
	{ "cancel", "", wl_touch_types + 14 },
	{ "shape", "6iff", wl_touch_types + 14 },
	{ "orientation", "6if", wl_touch_types + 18 },
};
const struct wl_interface wl_touch_interface = {
	"wl_touch", 10,
	1, wl_touch_requests,
	7, wl_touch_events
};

// >>> wl-stable-ifaces:END

// --- The stable-protocol interface tables (binary-local) --------------------------
//
// The retargeted backends' protocol inlines (the system's
// wayland-client-protocol.h + the embedded xdg block) take the core
// interfaces' DATA symbols' addresses by name; nothing links
// libwayland-client, so those extern references bind HERE (the dlopen'd
// library's own copies still gate the probe - the WL_IFACE set in
// wl_syms.h). Dumped from this box's libwayland-client.so.0 by
// dump_wl_ifaces.c (the authoring dumper, package root); rerunning it
// twice must leave this region unchanged.



// The soname candidate lists (ordered candidates per library; the first
// that dlopens wins). Single candidates - one stable soname across the
// whole in-range distro set.
static const char *const kWaylandClientNames[] = {"libwayland-client.so.0"};
static const char *const kXkbcommonNames[] = {"libxkbcommon.so.0"};
static const char *const kX11Names[] = {"libX11.so.6"};

// --- The dlopen/dlsym helpers -----------------------------------------------------

// Opens the first candidate that loads (RTLD_NOW: a broken .so is a probe
// failure, not a lazy crash later; RTLD_LOCAL: the windowing symbols must
// not shadow anything in the cradle); returns the handle or NULL.
static void *lx_dlopen(const char *const *names, int count, const char *what) {
  for (int i = 0; i < count; i++) {
    void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (h != NULL) {
      return h;
    }
  }
  LogError("[display_input_linux] dlopen failed for %s (%s): %s", what, names[0], dlerror());
  return NULL;
}

// Resolves one required symbol; a miss refuses the backend by name (the
// version-gated case: a symbol this build's protocol code needs that the
// runtime library lacks).
static void *lx_sym(void *h, const char *name, bool *ok) {
  void *p = dlsym(h, name);
  if (p == NULL) {
    if (*ok) {
      LogError("[display_input_linux] the loaded library is missing the required symbol %s (too old for the protocol code this build carries)", name);
    }
    *ok = 0;
  }
  return p;
}

// --- The per-library loaders (the X-macro lists drive them) ------------------------

// dlopen's wayland-client + xkbcommon and fills g_wl / g_xkb. Idempotent
// (the handles stick). Returns false on a missing .so or a missing
// REQUIRED symbol (the state is torn down; the probe moves on).
bool CruxLxProbe_LoadWayland(void) {
  bool ok = true;
  if (s_wl_lib == NULL) {
    s_wl_lib = lx_dlopen(kWaylandClientNames, 1, "wayland-client");
    if (s_wl_lib == NULL) {
      return false;
    }
  }
  if (s_xkb_lib == NULL) {
    s_xkb_lib = lx_dlopen(kXkbcommonNames, 1, "xkbcommon");
    if (s_xkb_lib == NULL) {
      return false;
    }
  }
  memset(&g_wl, 0, sizeof g_wl);
  memset(&g_xkb, 0, sizeof g_xkb);
#define WL_SYM(rc, fn, params) g_wl.fn = (rc(*)params)lx_sym(s_wl_lib, #fn, &ok);
#define WL_IFACE(name) g_wl.name = (const struct wl_interface *)lx_sym(s_wl_lib, #name, &ok);
#include "wl_syms.h"
#define XKB_SYM(rc, fn, params) g_xkb.fn = (rc(*)params)lx_sym(s_xkb_lib, #fn, &ok);
#include "xkb_syms.h"
  return ok;
}

// dlopen's libX11 and fills g_x11. Idempotent; same miss semantics.
bool CruxLxProbe_LoadX11(void) {
  bool ok = true;
  if (s_x11_lib == NULL) {
    s_x11_lib = lx_dlopen(kX11Names, 1, "libX11");
    if (s_x11_lib == NULL) {
      return false;
    }
  }
  memset(&g_x11, 0, sizeof g_x11);
#define X11_SYM(rc, fn, params) g_x11.fn = (rc(*)params)lx_sym(s_x11_lib, #fn, &ok);
#include "x11_syms.h"
  return ok;
}
