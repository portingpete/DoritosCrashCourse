#pragma once
#include "ppc_context.h"

namespace Doritos::Native {
// Runs on the title's guest GUI update thread, after the original GUI update.
void updateNativeGuiInput(PPCContext& ctx, uint8_t* base, uint32_t gui);
}
