#pragma once
#include "xex_loader.h"
struct PPCContext;
namespace Doritos::Native {
void initializeEnvironment(const LoadedXex& image);
bool environmentImport(PPCContext& ctx, uint8_t* base, const char* name);
}
