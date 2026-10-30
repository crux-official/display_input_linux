// The xkbcommon symbol table (ticket 0027 / 0030): the 8 symbols the
// wayland backend references (the keymap + the modifier-state sampler).
// X-macro shape: included three times (probe.h / probe.c / trampoline_wayland.h).
// All 8 carry the V_0.5.0 version tag on modern distros (verified on this
// box) - ancient and stable; the 0030 matrix confirms the floor.

#ifndef XKB_SYM
#define XKB_SYM(rc, fn, params)
#endif

XKB_SYM(struct xkb_context *, xkb_context_new, (enum xkb_context_flags flags))
XKB_SYM(struct xkb_keymap *, xkb_keymap_new_from_string, (struct xkb_context *context, const char *string, enum xkb_keymap_format format, enum xkb_keymap_compile_flags flags))
XKB_SYM(void, xkb_keymap_unref, (struct xkb_keymap *keymap))
XKB_SYM(struct xkb_state *, xkb_state_new, (struct xkb_keymap *keymap))
XKB_SYM(void, xkb_state_unref, (struct xkb_state *state))
XKB_SYM(enum xkb_state_component, xkb_state_update_key, (struct xkb_state *state, xkb_keycode_t key, enum xkb_key_direction dir))
XKB_SYM(enum xkb_state_component, xkb_state_update_mask, (struct xkb_state *state, xkb_mod_mask_t depressed_mods, xkb_mod_mask_t latched_mods, xkb_mod_mask_t locked_mods, xkb_layout_index_t depressed_layout, xkb_layout_index_t latched_layout, xkb_layout_index_t locked_layout))
XKB_SYM(int, xkb_state_mod_name_is_active, (struct xkb_state *state, const char *mod_name, enum xkb_state_component type))

#undef XKB_SYM
