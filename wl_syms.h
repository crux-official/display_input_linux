// The wayland-client symbol table: one line per symbol the wayland backend
// references at RUNTIME (the compiled-in protocol inlines included - they
// call the core API too).
//
// X-macro shape (the SDL3 SDL_waylandsym.h pattern): the list is included
// three times - the table-struct declaration (wl_tables.h), the loader
// (lx_probe.c), and the rename layer (trampoline_wayland.h). REQUIRED
// entries (WL_SYM) gate the probe: one missing symbol = this library
// version is incompatible with the protocol code we compiled = the backend
// is refused by name and the probe moves on.
//
// WHAT IS IN THE TABLE (the verified shape, this box = 1.24):
//   - the core display/proxy API the driver calls directly;
//   - the marshaling entry points the compiled-in protocol inlines call
//     (the VARIADIC ones: the scanner-generated code passes the args
//     variadically - the table entries must be variadic too, or the call
//     sites miscompile);
//   - wl_proxy_create / wl_proxy_destroy: the protocol inlines' hidden
//     core pull-ins (wl_display_bind itself is a header static inline -
//     NOT a dynamic symbol - and must stay unrenamed so the inline works);
//   - the wl_list_* API: extern functions in wayland-util.h (real exports),
//     required or the driver's list calls would link against nothing;
//   - the interface DATA symbols the protocol inlines take addresses of
//     (the probe GATE on the library's copies; the runtime data binds to
//     this package's binary-local generated region).
// The core PROTOCOL functions (wl_surface_commit, wl_shm_create_pool, ...)
// are header static inlines in EVERY wayland version - never dynamic
// symbols, never in the table.
//
// The REQUIRED set is the nm -u audit of the compiled backends on this box
// (the definitive list) and the soname lists are DATA.
// Every entry is REQUIRED: a miss refuses the backend by name (the safe
// failure mode) rather than degrading.

#ifndef WL_SYM
#define WL_SYM(rc, fn, params)
#endif
#ifndef WL_IFACE
#define WL_IFACE(name)
#endif

// --- the core display/proxy API (the driver's direct calls) -------------------
WL_SYM(struct wl_display *, wl_display_connect, (const char *name))
WL_SYM(void, wl_display_disconnect, (struct wl_display *display))
WL_SYM(int, wl_display_get_fd, (struct wl_display *display))
WL_SYM(int, wl_display_dispatch, (struct wl_display *display))
WL_SYM(int, wl_display_dispatch_pending, (struct wl_display *display))
WL_SYM(int, wl_display_prepare_read, (struct wl_display *display))
WL_SYM(int, wl_display_read_events, (struct wl_display *display))
WL_SYM(int, wl_display_flush, (struct wl_display *display))
WL_SYM(int, wl_display_get_protocol_error, (struct wl_display *display, const struct wl_interface **interface, uint32_t *id))
WL_SYM(int, wl_proxy_add_listener, (struct wl_proxy *proxy, void (**listener)(void), void *data))
WL_SYM(void, wl_proxy_set_user_data, (struct wl_proxy *proxy, void *data))
WL_SYM(void *, wl_proxy_get_user_data, (struct wl_proxy *proxy))
WL_SYM(uint32_t, wl_proxy_get_version, (struct wl_proxy *proxy))
// The modern marshaling entry the embedded protocol code's inlines call
// (the ONE version-gated symbol: absent before wayland 1.20):
WL_SYM(struct wl_proxy *, wl_proxy_marshal_flags, (struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, uint32_t version, uint32_t flags, ...))

// --- what the compiled-in protocol inlines pull in (the hidden core set) ------
WL_SYM(struct wl_proxy *, wl_proxy_create, (struct wl_proxy *parent, const struct wl_interface *interface))
WL_SYM(void, wl_proxy_destroy, (struct wl_proxy *proxy))
WL_SYM(struct wl_proxy *, wl_proxy_marshal, (struct wl_proxy *proxy, uint32_t opcode, ...))
WL_SYM(struct wl_proxy *, wl_proxy_marshal_constructor, (struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, ...))
WL_SYM(struct wl_proxy *, wl_proxy_marshal_constructor_versioned, (struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, uint32_t version, ...))

// --- the wl_list API (extern fns in wayland-util.h; wl_list_for_each is a macro) ---
WL_SYM(void, wl_list_init, (struct wl_list *list))
WL_SYM(void, wl_list_insert, (struct wl_list *list, struct wl_list *entry))
WL_SYM(void, wl_list_remove, (struct wl_list *entry))
WL_SYM(int, wl_list_length, (const struct wl_list *list))
WL_SYM(int, wl_list_empty, (const struct wl_list *list))
WL_SYM(void, wl_list_insert_list, (struct wl_list *list, struct wl_list *entry))

// --- the interface DATA symbols (the protocol inlines take their addresses) ---
WL_IFACE(wl_compositor_interface)
WL_IFACE(wl_shm_interface)
WL_IFACE(wl_seat_interface)
WL_IFACE(wl_keyboard_interface)
WL_IFACE(wl_pointer_interface)
WL_IFACE(wl_surface_interface)
WL_IFACE(wl_registry_interface)
WL_IFACE(wl_region_interface)
WL_IFACE(wl_buffer_interface)
WL_IFACE(wl_shm_pool_interface)
WL_IFACE(wl_output_interface)

#undef WL_SYM
#undef WL_IFACE
