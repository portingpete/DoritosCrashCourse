#pragma once
#include "bone_palette.h"

namespace Doritos::Graphics {
// Separate original RVMBatch producer contract; no Cal3D ownership inference.
struct TitleBatchGeometry {
    uint32_t verticesPerInstance, trianglesPerInstance, capacity, activeInstances;
};
struct TitleBatchDraw {
    TitleBatchGeometry geometry;
    std::span<const float> matrices;
};
inline uint32_t titleBatchVertexCount(const TitleBatchGeometry& draw);
inline uint32_t titleBatchBEWord(std::span<const uint8_t> bytes, uint32_t offset) {
    if (uint64_t(offset) + 4 > bytes.size()) throw std::runtime_error("Truncated original RVMBatch CPU descriptor");
    return uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset + 1]) << 16 |
        uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
}
inline uint64_t titleBatchBEHandle(std::span<const uint8_t> bytes, uint32_t offset) {
    return uint64_t(titleBatchBEWord(bytes,offset)) << 32 | titleBatchBEWord(bytes,offset + 4);
}
inline uint32_t titleBatchRegistryEntry(uint32_t renderer, uint32_t registry, uint64_t handle) {
    const int32_t index = static_cast<int16_t>((handle >> 16) & 0xffff);
    const uint64_t entry = uint64_t(renderer) + registry + (index < 0 ? 0 : uint32_t(index) * 24);
    if (!renderer || (registry != 0x11b90 && registry != 0x31778) || index < 0 || index >= 5000 || entry + 24 > (uint64_t(1) << 32))
        throw std::runtime_error("Original RVMBatch resource handle index exceeds its actual inline registry");
    return static_cast<uint32_t>(entry);
}
inline uint32_t titleBatchRegisteredObject(std::span<const uint8_t> entry, uint64_t handle) {
    if (entry.size() != 24 || titleBatchBEHandle(entry,16) != handle || !titleBatchBEWord(entry,8))
        throw std::runtime_error("Original RVMBatch full resource handle does not own its live CPU descriptor");
    return titleBatchBEWord(entry,8);
}
inline TitleBatchGeometry titleBatchCPUDescription(std::span<const uint8_t> batch, std::span<const uint8_t> material) {
    if (batch.size() < 248 || material.size() != 24 || titleBatchBEWord(batch,0) != 0x8215e29c)
        throw std::runtime_error("Unverified original RVMBatch CPU owner/material descriptor");
    const uint32_t begin = titleBatchBEWord(batch,224), end = titleBatchBEWord(batch,228), selected = titleBatchBEWord(batch,244);
    if (!begin || end <= begin || end - begin > 24 * 65536 || (end - begin) % 24 ||
        selected < begin || selected >= end || (selected - begin) % 24)
        throw std::runtime_error("Original RVMBatch selected material does not belong to its actual descriptor vector");
    const uint32_t capacity = (titleBatchBEWord(batch,200) & 0xffff), triangles = titleBatchBEWord(material,16) & 0xffff;
    const TitleBatchGeometry result{titleBatchBEWord(batch,204),triangles,capacity,titleBatchBEWord(batch,240)};
    if (uint64_t(capacity) * result.verticesPerInstance != titleBatchBEWord(batch,208))
        throw std::runtime_error("Original RVMBatch capacity disagrees with its actual vertex allocation count");
    titleBatchVertexCount(result);
    return result;
}
inline uint32_t titleBatchVertexCount(const TitleBatchGeometry& draw) {
    if (!draw.verticesPerInstance || !draw.trianglesPerInstance || !draw.capacity || draw.capacity > 32 ||
        !draw.activeInstances || draw.activeInstances > draw.capacity ||
        uint64_t(draw.capacity) * draw.verticesPerInstance > 64000 ||
        uint64_t(draw.capacity) * draw.trianglesPerInstance * 3 > 64000)
        throw std::runtime_error("Original RVMBatch exceeds its actual instance/16-bit geometry capacity");
    return draw.activeInstances * draw.verticesPerInstance;
}
inline uint32_t titleBatchIndexCount(const TitleBatchGeometry& draw) {
    titleBatchVertexCount(draw);
    return draw.activeInstances * draw.trianglesPerInstance * 3;
}
inline void validateTitleBatchParent(std::span<const uint8_t> parent, uint32_t owner) {
    if (parent.size() < 660 || !owner || titleBatchBEWord(parent,0) != 0x8215df5c ||
        titleBatchBEWord(parent,272) != 0x8215e040 || titleBatchBEWord(parent,648) != owner)
        throw std::runtime_error("Original RVMBatch does not belong to its live RenderVertexModel master");
}
inline void validateTitleBatchEngineBuffers(std::span<const uint8_t> vertex, std::span<const uint8_t> index,
    const TitleBatchGeometry& geometry, uint32_t vertexToken, uint32_t indexToken,
    uint32_t vertexBytes, uint32_t indexBytes) {
    titleBatchVertexCount(geometry);
    if (vertex.size() < 72 || index.size() < 64 || !vertexToken || !indexToken ||
        titleBatchBEWord(vertex,0) != 0x8216059c || titleBatchBEWord(index,0) != 0x821605b8 ||
        titleBatchBEWord(vertex,56) != 16 || titleBatchBEWord(vertex,60) != 84 ||
        titleBatchBEWord(vertex,64) != vertexToken || titleBatchBEWord(index,56) != indexToken ||
        // Original batch allocation822A8DA8 passes r6=1 to both Engine
        // virtual allocators: byte50 selects direct native buffer storage.
        // Their bind-time flush clears dirty byte49 while retaining byte50.
        vertex[49] || vertex[50] != 1 || index[49] || index[50] != 1 ||
        titleBatchBEWord(vertex,24) != geometry.capacity * geometry.verticesPerInstance ||
        titleBatchBEWord(index,24) != geometry.capacity * geometry.trianglesPerInstance ||
        titleBatchBEWord(vertex,20) < titleBatchBEWord(vertex,24) ||
        titleBatchBEWord(index,20) < titleBatchBEWord(index,24) ||
        uint64_t(titleBatchBEWord(vertex,20)) * 84 != vertexBytes ||
        uint64_t(titleBatchBEWord(index,20)) * 6 != indexBytes)
        throw std::runtime_error("Original RVMBatch does not own its actual clean format16 Engine buffers");
}
inline bool isTitleBatchDeclaration(std::span<const D3DVERTEXELEMENT9> declaration) {
    const D3DVERTEXELEMENT9 expected[] = {
        {0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},
        {0,12,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_BLENDWEIGHT,0},
        {0,28,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_BLENDINDICES,0},
        {0,44,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,0},
        {0,56,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_TANGENT,0},
        {0,72,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_COLOR,0},
        {0,76,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
    return declaration.size() == std::size(expected) && std::memcmp(declaration.data(),expected,sizeof(expected)) == 0;
}
inline void validateTitleBatchGeometry(const TitleBatchGeometry& draw, std::span<const uint8_t> nativeVertices,
    std::span<const uint16_t> indices) {
    const uint32_t vertexCount = titleBatchVertexCount(draw), indexCount = titleBatchIndexCount(draw);
    if (uint64_t(draw.capacity) * draw.verticesPerInstance * 84 > nativeVertices.size() ||
        uint64_t(draw.capacity) * draw.trianglesPerInstance * 3 > indices.size())
        throw std::runtime_error("Original RVMBatch actual native buffers do not hold their owned capacity");
    validateIndexedTriangleRange(D3DPT_TRIANGLELIST,0,vertexCount,vertexCount,indices,0,indexCount / 3);
    const uint32_t sourceIndexCount = draw.trianglesPerInstance * 3;
    // Native vectors stay immutable under the backend mutex for this call.
    // Replication is still checked for every index; only a repeated vertex's
    // four-lane proof is shared, and no proof survives the invocation.
    std::bitset<65536> validatedVertices;
    for (uint32_t offset = 0; offset < indexCount; ++offset) {
        const uint32_t ordinal = offset / sourceIndexCount, sourceIndex = indices[offset % sourceIndexCount];
        if (sourceIndex >= draw.verticesPerInstance ||
            indices[offset] != sourceIndex + ordinal * draw.verticesPerInstance)
            throw std::runtime_error("Original RVMBatch native indices do not preserve actual per-instance replication");
        const uint32_t vertex = indices[offset], vertexOrdinal = vertex / draw.verticesPerInstance;
        if (validatedVertices[vertex]) continue;
        for (uint32_t lane = 0; lane < 4; ++lane) {
            uint32_t weightBits, indexBits;
            std::memcpy(&weightBits,nativeVertices.data() + size_t(vertex) * 84 + 12 + lane * 4,4);
            std::memcpy(&indexBits,nativeVertices.data() + size_t(vertex) * 84 + 28 + lane * 4,4);
            const float matrix = std::bit_cast<float>(indexBits);
            // SkinVertex fetches all four lanes, including the three zero-weight lanes.
            if (weightBits != (lane ? 0u : 0x3f800000u) || !std::isfinite(matrix) ||
                matrix < 0 || matrix >= draw.activeInstances || std::floor(matrix) != matrix ||
                indexBits != std::bit_cast<uint32_t>(lane ? 0.0f : static_cast<float>(vertexOrdinal)))
                throw std::runtime_error("Original RVMBatch vertex does not own its actual one-hot active transform");
        }
        validatedVertices.set(vertex);
    }
}
inline uint32_t titleBatchCurrentPalette(ShaderStage stage, std::string_view name, const ShaderKey& key,
    const ShaderParameter& original, const ShaderParameter& native, std::span<const float> values,
    std::span<const bool> written, const TitleBatchDraw& draw) {
    titleBatchVertexCount(draw.geometry);
    // The original composite822D26B0 and multilayer822D05E8 material
    // entries both select the main-family skin VS for an active RVMBatch.
    // Their verified sources use the same four-index matrix loop.
    if (!isTitleBonePaletteShader(stage,name,key) || draw.matrices.size() != size_t(draw.geometry.activeInstances) * 16)
        throw std::runtime_error("Unverified original RVMBatch material/palette source");
    if (uint64_t(original.firstRegister) + original.registerCount > written.size() ||
        (uint64_t(original.firstRegister) + original.registerCount) * 4 > values.size())
        throw std::runtime_error("Original RVMBatch complete reflected palette exceeds actual constant spans");
    // The independent RVMBatch ownership/format16 geometry proof bounds all
    // four shader matrix indices to activeInstances, including zero-weight
    // lanes. Only that current renderer prefix is reachable. The common
    // binding/value check does not substitute Cal3D ownership for this proof.
    // Return the same checked row count to the actual native upload path;
    // unwritten reflected tail rows are neither required nor uploaded.
    return titleBonePalettePrefix(stage,name,key,original,native,values,written,draw.matrices);
}
}
