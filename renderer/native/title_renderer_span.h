#pragma once
#include "guest_span.h"

namespace Doritos::Graphics {
constexpr uint32_t titleRendererOwnedBytes = 0x56b00;
constexpr uint32_t titleRendererPaletteOffset = 0x517e0;
constexpr uint32_t titleRendererCullCameraOffset = 0x11950;
constexpr uint32_t titleRendererCullModeOffset = 0x5654c;
constexpr uint32_t titleRendererCullPrefixBytes = titleRendererCullModeOffset + 4;
static_assert(0x11b90 + 5000 * 24 <= titleRendererOwnedBytes);
static_assert(0x31778 + 5000 * 24 <= titleRendererOwnedBytes);
static_assert(titleRendererPaletteOffset + 48 * 64 <= titleRendererOwnedBytes);
static_assert(titleRendererCullCameraOffset + 4 <= titleRendererCullPrefixBytes);
static_assert(titleRendererCullPrefixBytes <= titleRendererOwnedBytes);
// This checks logical owner bounds only. The bridge must freshly validate the
// returned field's actual bytes before reading or writing them.
inline uint32_t titleRendererFieldAddress(uint32_t rendererAddress,uint32_t offset,uint32_t count) {
    constexpr uint64_t addressSpace = uint64_t(1) << 32;
    if (rendererAddress < 0x10000 || uint64_t(rendererAddress) + titleRendererOwnedBytes > addressSpace ||
        !count || offset > titleRendererOwnedBytes || count > titleRendererOwnedBytes - offset)
        throw std::runtime_error("Original renderer field exceeds its logical owner");
    return rendererAddress + offset;
}
// Caller validates this exact original renderer once for the current draw.
// Only inline bytes are shared; registry pointees remain independently checked.
inline std::span<const uint8_t> titleRendererOwnedRange(std::span<const uint8_t> renderer,
    uint32_t rendererAddress, uint64_t address, uint32_t count) {
    if (renderer.size() != titleRendererOwnedBytes)
        throw std::runtime_error("Original renderer owner span has the wrong bounds");
    return titleGuestOwnedRange(renderer,rendererAddress,address,count);
}
inline std::span<const uint8_t> titleRendererPalette(std::span<const uint8_t> renderer,
    uint32_t rendererAddress, uint32_t matrices, uint32_t maximumMatrices) {
    // The distinct RVMBatch and Cal3D producers own at most32/48 float4x4s.
    if ((maximumMatrices != 32 && maximumMatrices != 48) || !matrices || matrices > maximumMatrices)
        throw std::runtime_error("Original renderer palette exceeds its producer bounds");
    return titleRendererOwnedRange(renderer,rendererAddress,
        uint64_t(rendererAddress) + titleRendererPaletteOffset,matrices * 64);
}
}
