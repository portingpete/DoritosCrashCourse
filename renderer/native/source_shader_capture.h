#pragma once
#include "source_shader.h"

namespace Doritos::Graphics {
// Opt-in private diagnostic only; no token, compile policy or shader selection changes.
void captureOriginalSourceShader(const ShaderSelection& selection, ShaderStage stage,
    uint32_t master, uint32_t variantAddress, std::span<const uint8_t> variant,
    std::span<const uint8_t> code);
}
