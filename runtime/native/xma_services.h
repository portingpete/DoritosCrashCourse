#pragma once
#include "ppc_context.h"
#include <cstdint>

namespace Doritos::Native {
class Memory;
bool xmaServicesImport(PPCContext& ctx, uint8_t* base, const char* name);
bool xmaReadMmio(uint32_t address, unsigned width, uint64_t& value);
bool xmaWriteMmio(uint32_t address, uint64_t value, unsigned width);
void shutdownXmaServices(Memory* owner);
}
