#pragma once
#include "ppc_context.h"
#include <cstdint>

namespace Doritos::Native {
class Memory;
constexpr uint32_t audioChannels = 6, audioFrames = 256, audioRate = 48000;
constexpr uint32_t audioFrameBytes = audioChannels * audioFrames * 4;
void convertAudioPlanarBE(const uint8_t* source, float* output);
bool audioServicesImport(PPCContext& ctx, uint8_t* base, const char* name);
// Terminal process-close request: mute/cancel callbacks and reject new audio
// work, retaining all storage until coordinated shutdown or process termination.
void requestAudioServicesStop(Memory* owner);
void shutdownAudioServices(Memory* owner);
}
