// The backend probe of display_input_linux: the package-internal surface
// (the display half's probe semantics - the order, the env knob, the
// winner). The dlopen machinery (the symbol tables, the X-macro lists,
// the trampoline rename layer, the generic loaders) is lx_probe.c in
// this package; the probe entry points (DlWayland_Probe / DlX11_Probe)
// are defined by the backends.
//
// The probe runs ONCE, at the package's construct, and the winner is a
// process-lifetime choice. It sets a TENTATIVE winner before each real
// connection test (the wayland connect registers the input feed hook
// mid-probe, which dispatches through the input half and reads this
// global) and rolls it back on failure.

#ifndef CRUX_LINUX_PROBE_H
#define CRUX_LINUX_PROBE_H

#include "kernel_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The backend enum (process-lifetime winner). The order is the PROBE
// ORDER (today: wayland, x11, none).
enum {
  CRUX_LNX_NONE = 0, // the none fallback (the display_none/input_none contract)
  CRUX_LNX_WAYLAND,
  CRUX_LNX_X11,
};

// The env-override knob: CRUX_DISPLAY_BACKEND auto|wayland|x11|none - a
// forced-unavailable backend is a hard none, an invalid value logs + auto.
#define CRUX_LNX_ENV_KNOB "CRUX_DISPLAY_BACKEND"

// --- The query ------------------------------------------------------------------

// Runs the probe exactly once (idempotent); returns the winner enum. The
// probe is triggered lazily by the FIRST caller (the package's construct),
// so no caller order matters.
uint32_t CruxDisplayLinux_EnsureProbed(void);

// The winner WITHOUT triggering the probe (0 = not probed yet - callers
// must EnsureProbed first; the construct does).
uint32_t CruxDisplayLinux_Backend(void);

// The backend's probe entry points (defined by the backends; probe.c
// calls them after the symbol tables resolve):
//   - DlWayland_Probe: the wayland connect (connect + registry + globals
//     check + the feed hook hand-off); the connection, when successful,
//     STAYS up (the driver reuses it - its lazy connect is idempotent).
//   - DlX11_Probe: the x11 display open (XOpenDisplay + the visual floor
//     check); the connection, when successful, stays up likewise.
// Each returns false on any miss (the state is torn down; no retry later -
// the probe ran once, process lifetime).
bool DlWayland_Probe(void);
bool DlX11_Probe(void);

#ifdef __cplusplus
}
#endif

#endif
