// The backend probe SEMANTICS of display_input_linux.
//
// The dlopen machinery (the per-library symbol tables, the X-macro symbol
// lists, the generic loaders) is lx_probe.c in this package. This file
// owns what the display half owns: the probe ORDER, the env knob, the
// winner state, and the tentative winner + rollback.
//
// The probe runs ONCE (process lifetime), triggered by the package's
// construct (the engine walks it once). It sets a TENTATIVE winner before
// each real connection test (the wayland connect registers the input feed
// hook mid-probe, which dispatches through the input half and reads this
// global) and rolls it back on failure.

#include "probe.h"
#include "lx_probe.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

static uint32_t s_backend = CRUX_LNX_NONE;
static int s_probed = 0;

// The env-knob read (CRUX_DISPLAY_BACKEND, auto|wayland|x11|none; a
// forced-unavailable backend is a hard none, an invalid value logs + auto):
static int lx_knob(void) {
  const char *v = getenv(CRUX_LNX_ENV_KNOB);
  if (v == NULL || v[0] == '\0' || strcmp(v, "auto") == 0) {
    return -1; // auto
  }
  if (strcmp(v, "wayland") == 0) {
    return CRUX_LNX_WAYLAND;
  }
  if (strcmp(v, "x11") == 0) {
    return CRUX_LNX_X11;
  }
  if (strcmp(v, "none") == 0) {
    return CRUX_LNX_NONE;
  }
  LogError("[display_input_linux] %s=%s is not auto|wayland|x11|none; treating as auto", CRUX_LNX_ENV_KNOB, v);
  return -1;
}

static bool lx_tryBackend(uint32_t backend) {
  // Set the TENTATIVE winner before the connection test: the wayland
  // connect registers the input feed hook mid-probe, which dispatches
  // through the input half and reads the winner. Roll back on failure.
  s_backend = backend;
  bool ok = false;
  switch (backend) {
  case CRUX_LNX_WAYLAND:
    ok = CruxLxProbe_LoadWayland() && DlWayland_Probe();
    if (!ok) {
      LogError("[display_input_linux] the wayland backend is unavailable on this box");
    }
    break;
  case CRUX_LNX_X11:
    ok = CruxLxProbe_LoadX11() && DlX11_Probe();
    if (!ok) {
      LogError("[display_input_linux] the x11 backend is unavailable on this box");
    }
    break;
  default:
    break;
  }
  if (!ok) {
    s_backend = CRUX_LNX_NONE; // the rollback
  }
  return ok;
}

static uint32_t run_probe(void) {
  const int forced = lx_knob();
  if (forced == CRUX_LNX_NONE) {
    LogWarning("[display_input_linux] %s=none: the display concern is disabled for this run", CRUX_LNX_ENV_KNOB);
    return CRUX_LNX_NONE;
  }
  if (forced == CRUX_LNX_WAYLAND || forced == CRUX_LNX_X11) {
    if (lx_tryBackend((uint32_t)forced)) {
      LogWarning("[display_input_linux] %s=%s: the backend is forced", CRUX_LNX_ENV_KNOB, forced == CRUX_LNX_WAYLAND ? "wayland" : "x11");
      return (uint32_t)forced;
    }
    LogError("[display_input_linux] %s forced an unavailable backend; falling back to none", CRUX_LNX_ENV_KNOB);
    return CRUX_LNX_NONE;
  }
  // The probe order: wayland, then x11, then none:
  if (lx_tryBackend(CRUX_LNX_WAYLAND)) {
    return CRUX_LNX_WAYLAND;
  }
  if (lx_tryBackend(CRUX_LNX_X11)) {
    return CRUX_LNX_X11;
  }
  LogWarning("[display_input_linux] no display backend reachable (no wayland, no x11); running the none fallback");
  return CRUX_LNX_NONE;
}

// --- The cross-package API ------------------------------------------------------------

uint32_t CruxDisplayLinux_EnsureProbed(void) {
  if (!s_probed) {
    s_probed = 1;
    s_backend = run_probe();
    Log("[display_input_linux] the backend probe picked: %s",
        s_backend == CRUX_LNX_WAYLAND ? "wayland" : s_backend == CRUX_LNX_X11 ? "x11" : "none");
  }
  return s_backend;
}

uint32_t CruxDisplayLinux_Backend(void) {
  return s_backend;
}
