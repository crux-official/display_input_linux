// The wayland-side symbol tables (ticket 0027) - package-internal.
//
// Self-contained: this header pulls the type headers itself (the table
// prototypes name their types). Driver TUs still include the type headers
// FIRST (before the trampoline / protocol headers) for the protocol
// inlines' include-order contract - this header's own include is then a
// guarded no-op. The X11 table is a separate header (x11_tables.h) so a
// wayland-only TU never parses X11 types - and vice versa.

#ifndef CRUX_LINUX_WL_TABLES_H
#define CRUX_LINUX_WL_TABLES_H

#include <wayland-client-core.h>
#include <xkbcommon/xkbcommon.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WlTable {
#define WL_SYM(rc, fn, params) rc (*fn) params;
#define WL_IFACE(name) const struct wl_interface *name;
#include "wl_syms.h"
} WlTable;

typedef struct XkbTable {
#define XKB_SYM(rc, fn, params) rc (*fn) params;
#include "xkb_syms.h"
} XkbTable;

// The resolved tables (probe.c defines; the trampoline headers reference
// them as externs).
extern WlTable g_wl;
extern XkbTable g_xkb;

#ifdef __cplusplus
}
#endif

#endif
