#pragma once
#include "ppc_context.h"

namespace Doritos::Native {
// Only the newly authored, named Help & Options tile owns this command.
// Returns true when handled (including safely rejected stale invocations).
bool handleNativeVideoMenuCommand(PPCContext& ctx, uint8_t* base);
}
