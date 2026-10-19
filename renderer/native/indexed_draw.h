#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <d3d9.h>

namespace Doritos::Graphics {
struct TitleIndexedDraw {
    D3DPRIMITIVETYPE topology;
    uint32_t triangles;
};
// Original renderer8234D4A8/8234D5D0 expand logical triangle counts using
// table8213D680: Xbox type4 consumes3*N indices; type6 consumesN+2.
// The Xbox strip enum6 differs from native D3D9's strip enum5.
inline TitleIndexedDraw decodeTitleIndexedDraw(uint32_t xboxPrimitive, uint32_t indexCount) {
    if (xboxPrimitive == 4 && indexCount && indexCount % 3 == 0)
        return {D3DPT_TRIANGLELIST,indexCount / 3};
    if (xboxPrimitive == 6 && indexCount >= 3)
        return {D3DPT_TRIANGLESTRIP,indexCount - 2};
    throw std::runtime_error("Unverified original title indexed primitive/count");
}
inline uint32_t validateIndexedTriangleRange(D3DPRIMITIVETYPE topology, int32_t baseVertex,
    uint32_t vertexCount, uint32_t availableVertices, std::span<const uint16_t> indices,
    uint32_t startIndex, uint32_t triangles) {
    if (!triangles || !vertexCount || baseVertex < 0 || uint32_t(baseVertex) > availableVertices ||
        vertexCount > availableVertices - uint32_t(baseVertex))
        throw std::runtime_error("Native indexed title draw exceeds vertex resource bounds");
    const uint64_t consumed = topology == D3DPT_TRIANGLELIST ? uint64_t(triangles) * 3 :
        topology == D3DPT_TRIANGLESTRIP ? uint64_t(triangles) + 2 : 0;
    if (!consumed || startIndex > indices.size() || consumed > indices.size() - startIndex)
        throw std::runtime_error("Native indexed title draw exceeds index resource bounds");
    for (uint64_t index = startIndex; index < uint64_t(startIndex) + consumed; ++index)
        if (indices[static_cast<std::size_t>(index)] >= vertexCount)
            throw std::runtime_error("Original title index references a vertex outside the native draw range");
    return static_cast<uint32_t>(consumed);
}
}
