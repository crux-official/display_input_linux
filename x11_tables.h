// The x11-side symbol table (ticket 0027) - package-internal.
//
// Self-contained: this header pulls the X11 type headers itself (the
// table prototypes name their types - XSizeHints/XVisualInfo in Xutil.h,
// Atom in Xatom.h). Driver TUs still include the type headers FIRST
// (before the trampoline) for the rename layer's include-order contract -
// this header's own includes are then guarded no-ops.

#ifndef CRUX_LINUX_X11_TABLES_H
#define CRUX_LINUX_X11_TABLES_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct X11Table {
#define X11_SYM(rc, fn, params) rc (*fn) params;
#include "x11_syms.h"
} X11Table;

extern X11Table g_x11;

#ifdef __cplusplus
}
#endif

#endif
