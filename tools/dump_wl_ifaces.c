// The authoring pipeline for the stable-protocol interface tables embedded
// in src/lx_probe.c (the region between the wl-stable-ifaces:BEGIN /
// wl-stable-ifaces:END markers): dlopen the machine's libwayland-client.so.0,
// walk the named interfaces, and emit C definitions (the wayland-scanner
// private-code shape) so the retargeted backends' protocol inlines bind to
// BINARY-LOCAL data - nothing links libwayland-client (the library is
// reached only through the g_wl table), so the inlines' extern references
// must resolve inside the cradle.
//
// Usage (at authoring time, on the box whose library the build targets):
//   cc -O1 -o dump_wl_ifaces dump_wl_ifaces.c -ldl
//   ./dump_wl_ifaces <scratch-out.h>
//   splice the emitted marked region into src/lx_probe.c's marked region
//   (the output is emitted with the markers, ready to paste).
//
// Layout: one flat <iface>_types[] array per interface - the requests'
// signature slots (in order) followed by the events' (in order); each
// message's types pointer restarts at its slice's offset. A message with a
// signature of N characters owns N slots (the library's invariant - the
// protocol wire format's one-type-per-argument encoding).
//
// Verify: an interface whose types reference an interface outside the
// dumped set FAILS the dump by name (add it to kIfaceNames and rerun); a
// library layout drift reads garbage names and the per-symbol name check
// fails loudly. Run it twice; the second run must leave the region
// unchanged (the embedded bytes are a pure function of the machine's
// library). The generated block is committed (a standalone's cradle entry
// carries .c + .extension only, and the consumer's build must compile
// exactly the pinned bytes - it never runs this dumper).

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The machine's wayland-client-core.h layout (frozen by the library's
// ABI - a layout drift reads garbage names and the name check below fails
// the dump loudly).
typedef struct msg {
  const char *name;
  const char *signature;
  const void **types;
} msg_t;
typedef struct iface {
  const char *name;
  uint32_t version;
  uint32_t method_count;
  const msg_t *methods;
  uint32_t event_count;
  const msg_t *events;
} iface_t;

// The stable core interfaces the retargeted backends' protocol inlines
// reference (the ten WL_IFACE table entries + wl_output - the embedded
// xdg types array names it - + wl_touch - wl_seat's get_touch names it -
// + wl_callback - this box's wayland's extended format marks the callback
// arguments, and the dumper's fail-loud set check names every one).
static const char *const kIfaceNames[] = {
    "wl_buffer", "wl_callback", "wl_compositor", "wl_keyboard", "wl_output",
    "wl_pointer", "wl_region", "wl_registry", "wl_seat", "wl_shm",
    "wl_shm_pool", "wl_surface", "wl_touch",
};
#define K_N (sizeof kIfaceNames / sizeof kIfaceNames[0])

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <out.h>\n", argv[0]);
    return 2;
  }
  void *h = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
  if (h == NULL) {
    fprintf(stderr, "dlopen failed: %s\n", dlerror());
    return 1;
  }
  iface_t *ifs[K_N];
  for (size_t i = 0; i < K_N; i++) {
    char sym[128];
    snprintf(sym, sizeof sym, "%s_interface", kIfaceNames[i]);
    void *p = dlsym(h, sym);
    if (p == NULL) {
      fprintf(stderr, "the library is missing the data symbol %s\n", sym);
      return 1;
    }
    ifs[i] = (iface_t *)p;
    if (strcmp(ifs[i]->name, kIfaceNames[i]) != 0) {
      fprintf(stderr, "the symbol %s names itself %s (a library layout drift - the dumper's assumptions are stale)\n", sym, ifs[i]->name);
      return 1;
    }
  }
  FILE *f = fopen(argv[1], "w");
  if (f == NULL) {
    fprintf(stderr, "cannot write %s\n", argv[1]);
    return 1;
  }
  fprintf(f, "// >>> wl-stable-ifaces:BEGIN (generated - do not edit; the dumper is packages/lx_probe/dump_wl_ifaces.c)\n");
  // Forward declarations (the definitions below reference each other -
  // the system's wayland-client-protocol.h carries the identical externs):
  for (size_t i = 0; i < K_N; i++) {
    fprintf(f, "extern const struct wl_interface %s_interface;\n", kIfaceNames[i]);
  }
  for (size_t i = 0; i < K_N; i++) {
    iface_t *ifc = ifs[i];
    const msg_t *lists[2] = {ifc->methods, ifc->events};
    uint32_t counts[2] = {ifc->method_count, ifc->event_count};
    // The flat types array (requests' slots, then events').
    size_t nslots = 0;
    for (int li = 0; li < 2; li++) {
      for (uint32_t c = 0; c < counts[li]; c++) {
        nslots += strlen(lists[li][c].signature);
      }
    }
    // Always emitted: the message slices' offsets index it even for
    // signature-less messages (a single NULL placeholder keeps +0 legal).
    fprintf(f, "static const struct wl_interface *%s_types[] = {\n", kIfaceNames[i]);
    if (nslots == 0) {
      fprintf(f, "\tNULL,\n");
    }
    {
      for (int li = 0; li < 2; li++) {
        for (uint32_t c = 0; c < counts[li]; c++) {
          const msg_t *m = &lists[li][c];
          for (size_t j = 0; j < strlen(m->signature); j++) {
            const void *tp = m->types[j];
            if (tp == NULL) {
              fprintf(f, "\tNULL,\n");
              continue;
            }
            size_t ti = 0;
            for (ti = 0; ti < K_N; ti++) {
              if (ifs[ti] == (iface_t *)tp) {
                break;
              }
            }
            if (ti == K_N) {
              fprintf(stderr, "the interface %s's message %s (a %s) references the interface at %p, which is not in the dumped set (add it to kIfaceNames)\n", kIfaceNames[i], m->name, li == 0 ? "request" : "event", tp);
              fclose(f);
              return 1;
            }
            fprintf(f, "\t&%s_interface,\n", kIfaceNames[ti]);
          }
        }
      }
      fprintf(f, "};\n");
    }
    size_t req_off = 0;
    if (ifc->method_count > 0) {
      fprintf(f, "static const struct wl_message %s_requests[] = {\n", kIfaceNames[i]);
      for (uint32_t m = 0; m < ifc->method_count; m++) {
        fprintf(f, "\t{ \"%s\", \"%s\", %s_types + %zu },\n", ifc->methods[m].name, ifc->methods[m].signature, kIfaceNames[i], req_off);
        req_off += strlen(ifc->methods[m].signature);
      }
      fprintf(f, "};\n");
    }
    if (ifc->event_count > 0) {
      size_t evt_off = 0;
      fprintf(f, "static const struct wl_message %s_events[] = {\n", kIfaceNames[i]);
      for (uint32_t e = 0; e < ifc->event_count; e++) {
        fprintf(f, "\t{ \"%s\", \"%s\", %s_types + %zu },\n", ifc->events[e].name, ifc->events[e].signature, kIfaceNames[i], req_off + evt_off);
        evt_off += strlen(ifc->events[e].signature);
      }
      fprintf(f, "};\n");
    }
    if (ifc->method_count > 0) {
      fprintf(f, "const struct wl_interface %s_interface = {\n\t\"%s\", %u,\n\t%u, %s_requests,\n", kIfaceNames[i], ifc->name, ifc->version, ifc->method_count, kIfaceNames[i]);
    } else {
      fprintf(f, "const struct wl_interface %s_interface = {\n\t\"%s\", %u,\n\t0, NULL,\n", kIfaceNames[i], ifc->name, ifc->version);
    }
    if (ifc->event_count > 0) {
      fprintf(f, "\t%u, %s_events\n};\n\n", ifc->event_count, kIfaceNames[i]);
    } else {
      fprintf(f, "\t0, NULL\n};\n\n");
    }
  }
  fprintf(f, "// >>> wl-stable-ifaces:END\n");
  fclose(f);
  return 0;
}
