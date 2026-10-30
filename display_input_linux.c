// The extension face of display_input_linux: display + input as one package. Construct probes once and,
// only if a backend won, registers the concern bits and served structs (none contract: no bit, no struct, the app's guards hold).
#include "extension.h"
#include "display_api.h"
#include "input_api.h"
#include "probe.h"

#include <stdbool.h>

// The halves' served structs (each half's TU owns its dispatch + struct).
extern DisplayAPI g_display_api;
extern InputAPI g_input_api;

static bool display_input_linux_construct(void) {
  if (CruxDisplayLinux_EnsureProbed() != CRUX_LNX_NONE) {
    Engine_RegisterConcern(CRUX_CONCERN_DISPLAY | CRUX_CONCERN_INPUT);
    Engine_RegisterConcernAPI("display", &g_display_api);
    Engine_RegisterConcernAPI("input", &g_input_api);
  }
  return true;
}

ExtensionInterface g_extension_display_input_linux = {
    "display_input_linux",
    {1, 0, 0},
    display_input_linux_construct,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};
