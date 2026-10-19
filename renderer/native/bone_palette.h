#pragma once
#include "indexed_draw.h"
#include "shader_cache.h"
#include <bit>
#include <bitset>
#include <cmath>
#include <cstring>
#include <string_view>

namespace Doritos::Graphics {
struct TitleBoneGeometry {
    uint32_t totalVertices, totalTriangles, baseVertex, vertexCount, startIndex, indexCount, boneCount;
};
struct TitleBoneDraw {
    TitleBoneGeometry geometry;
    std::span<const float> matrices;
};
inline void validateTitleBoneExporterBinding(std::span<const uint8_t> exporter, uint32_t vertexData, uint32_t indexData) {
    if (exporter.size() < 224) throw std::runtime_error("Original Cal3D exporter descriptor is truncated");
    const auto word = [&](uint32_t offset) {
        return uint32_t(exporter[offset]) << 24 | uint32_t(exporter[offset + 1]) << 16 |
            uint32_t(exporter[offset + 2]) << 8 | exporter[offset + 3];
    };
    // Original8239EDF8 retargets only56/60 and220 after the native buffer
    // copy. Secondary attribute pointers64/72/80 retain freed CPU staging;
    // they neither authorize this draw nor supply the checked vertex bytes.
    if (!vertexData || !indexData || word(56) != vertexData || word(60) != 80 || word(220) != indexData)
        throw std::runtime_error("Original Cal3D exporter does not own the actually bound native geometry data");
}
inline bool isTitleBonePaletteShader(ShaderStage stage, std::string_view name, const ShaderKey& key) {
    // Both verified original main sources access g_skinMatrix only through
    // the same four-index loop. Original feature table word2 bit6 is exactly
    // _USE_SKINMATRIX. The backend additionally requires immutable original
    // and native cache identity plus the exact reflected parameter binding.
    return stage == ShaderStage::Vertex && (name == "multilayer/vs" || name == "composite/vs") && (key[2] & 64);
}
inline bool isTitleBonePaletteBinding(ShaderStage stage, std::string_view name, const ShaderKey& key,
    const ShaderParameter& original, const ShaderParameter& native) {
    return isTitleBonePaletteShader(stage,name,key) && original.id == 81 && native.id == 81 &&
        !original.reserved && !native.reserved && original.registerCount == 192 && native.registerCount == 192 &&
        original.firstRegister <= 1024 - 192 && native.firstRegister <= 256 - 192;
}
inline bool isTitleBoneDeclaration(std::span<const D3DVERTEXELEMENT9> declaration) {
    const D3DVERTEXELEMENT9 expected[] = {
        {0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},
        {0,12,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_BLENDWEIGHT,0},
        {0,28,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_BLENDINDICES,0},
        {0,44,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,0},
        {0,56,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_TANGENT,0},
        {0,72,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
    return declaration.size() == std::size(expected) &&
        std::memcmp(declaration.data(),expected,sizeof(expected)) == 0;
}
inline void validateTitleBoneGeometry(const TitleBoneGeometry& draw, std::span<const uint8_t> nativeVertices,
    std::span<const uint16_t> indices) {
    if (!draw.boneCount || draw.boneCount > 48 || !draw.totalVertices || !draw.totalTriangles ||
        !draw.vertexCount || !draw.indexCount || draw.indexCount % 3 ||
        uint64_t(draw.baseVertex) + draw.vertexCount > draw.totalVertices ||
        uint64_t(draw.startIndex) + draw.indexCount > uint64_t(draw.totalTriangles) * 3 ||
        uint64_t(draw.totalVertices) * 80 > nativeVertices.size() ||
        uint64_t(draw.totalTriangles) * 3 > indices.size())
        throw std::runtime_error("Original Cal3D group exceeds its real geometry or 48-bone palette bounds");
    validateIndexedTriangleRange(D3DPT_TRIANGLELIST,static_cast<int32_t>(draw.baseVertex),draw.vertexCount,
        draw.totalVertices,indices,draw.startIndex,draw.indexCount / 3);
    // The backend holds its mutex while these native vectors are immutable.
    // Only this invocation shares a referenced vertex's four-lane proof;
    // the uint16 relative index remains bounded even with a nonzero base.
    std::bitset<65536> validatedVertices;
    for (uint32_t offset = 0; offset < draw.indexCount; ++offset) {
        const uint16_t relativeVertex = indices[draw.startIndex + offset];
        if (validatedVertices[relativeVertex]) continue;
        const uint32_t vertex = draw.baseVertex + relativeVertex;
        for (uint32_t lane = 0; lane < 4; ++lane) {
            float bone;
            std::memcpy(&bone,nativeVertices.data() + size_t(vertex) * 80 + 28 + lane * 4,4);
            // SkinVertex reads all four indices before multiplying by weights.
            if (!std::isfinite(bone) || bone < 0 || bone >= draw.boneCount || std::floor(bone) != bone)
                throw std::runtime_error("Original Cal3D vertex addresses an unavailable active bone matrix");
        }
        validatedVertices.set(relativeVertex);
    }
}
inline uint32_t titleBonePalettePrefix(ShaderStage stage, std::string_view name, const ShaderKey& key,
    const ShaderParameter& original, const ShaderParameter& native, std::span<const float> values,
    std::span<const bool> written, std::span<const float> matrices) {
    if (!isTitleBonePaletteBinding(stage,name,key,original,native) || matrices.empty() ||
        matrices.size() % 16 || matrices.size() > 48 * 16)
        throw std::runtime_error("Unverified native Cal3D active palette shader binding");
    const uint32_t rows = static_cast<uint32_t>(matrices.size() / 4);
    if (original.firstRegister + rows > written.size() ||
        size_t(original.firstRegister) * 4 + matrices.size() > values.size())
        throw std::runtime_error("Original Cal3D uploaded palette prefix is outside constant bounds");
    for (uint32_t row = 0; row < rows; ++row)
        if (!written[original.firstRegister + row])
            throw std::runtime_error("Original title did not upload its active Cal3D matrix prefix");
    for (size_t index = 0; index < matrices.size(); ++index)
        if (std::bit_cast<uint32_t>(values[original.firstRegister * 4 + index]) != std::bit_cast<uint32_t>(matrices[index]))
            throw std::runtime_error("Original Cal3D uploaded matrix prefix does not match the current group palette");
    return rows; // Upload only actual rows; reflected but unreachable tail remains untouched.
}
}
