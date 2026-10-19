#pragma once
#include "d3d9_backend.h"
#include "depth_target.h"
#include <span>

struct PPCContext;

namespace Doritos::Graphics {
// Root must verify/decode the two original inputs from the user's supported
// encrypted game and pin any derived extension before installing strong hooks.
void prepareTitleGraphics(std::span<const uint8_t> windowsCache, std::span<const uint8_t> xenonCache, std::span<const uint8_t> nativeExtension = {}, std::span<const uint8_t> xenonSourceExtension = {}, HANDLE closeRequested = nullptr);
void shutdownTitleGraphics();
RenderCounters titleGraphicsCounters();
void prepareTitleFrame();
void prepareTitleSceneResolution(uint32_t renderer);
void updateTitleRenderResolution(PPCContext& context, uint8_t* base, uint32_t renderer,
    uint32_t width, uint32_t height);
// Unwired native allocation helper. The actual type1 constructor/End need
// their separately verified integration before these resources can be drawn.
// Returns an empty pair after genuine native/host allocation exhaustion.
// Contract/capability/device failures throw; original owner bytes stay intact.
TitleDepthTargetResources createTitleDepthTargetResources(uint32_t owner);
}
