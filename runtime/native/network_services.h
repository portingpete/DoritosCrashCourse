#pragma once
#include "ppc_context.h"
namespace Doritos::Native {
bool networkServicesImport(PPCContext& ctx, uint8_t* base, const char* name);
void shutdownNetworkServices();
}
