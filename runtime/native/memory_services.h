#pragma once
#include "ppc_context.h"
#include <cstdint>

namespace Doritos::Native {
void initializeMemoryServices();
bool memoryServicesImport(PPCContext& ctx, uint8_t* base, const char* name);
// Resolve native CPU-owned physical backing; never invent a cache/device alias.
uint32_t resolvePhysicalAddress(uint32_t physical, uint32_t bytes);
}
