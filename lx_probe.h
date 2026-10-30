// The dlopen machinery's internal contract (the package is one static
// cradle binary - plain direct calls, no dispatch).
//
// The symbol tables themselves (g_wl / g_xkb / g_x11) are declared in
// wl_tables.h / x11_tables.h (the driver TUs include those, after the
// backend type headers - see each header's include-order contract); the
// trampoline rename layer is trampoline_wayland.h / trampoline_x11.h.
// This header is the ONLY thing the probe-logic side (probe.c) needs:
// the generic loaders.

#ifndef CRUX_LX_PROBE_H
#define CRUX_LX_PROBE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// dlopen's wayland-client + xkbcommon and fills the g_wl / g_xkb tables.
// Idempotent (the handles stick). Returns false on a missing .so or a
// missing REQUIRED symbol (the version-gate: the probe then moves on to
// the next backend by name).
bool CruxLxProbe_LoadWayland(void);

// dlopen's libX11 and fills the g_x11 table; same idempotence and miss
// semantics.
bool CruxLxProbe_LoadX11(void);

#ifdef __cplusplus
}
#endif

#endif
