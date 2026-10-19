#pragma once
#include "ppc_context.h"
#include <cstdint>
#include <filesystem>

namespace Doritos::Native {
// Activates the actual Windows account as this port's local, offline profile.
// Its randomly generated local ID is persisted beneath the supplied save root.
// Normal startup selects that account immediately. False retains the optional
// explicit local-profile chooser, including its real Use/Cancel lifecycle.
void initializeUserServices(const std::filesystem::path& saveRoot, bool selectAccount = true);
void shutdownUserServices();
// Read-only identity of the actual Windows account selected at startup or
// through the owned local-profile dialog; zero means no selection.
uint64_t nativeSelectedLocalProfile(uint32_t user);
bool userServicesImport(PPCContext& ctx, uint8_t* base, const char* name);
}
