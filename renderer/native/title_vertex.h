#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include <d3d9.h>

namespace Doritos::Graphics {
// This title builds 12-byte Xbox declaration records in 0x82384D60 and its
// helpers; stream/offset are BE16, type is BE32, method/usage/index are bytes.
std::vector<D3DVERTEXELEMENT9> decodeTitleDeclaration(std::span<const uint8_t> records);
// Exact first SDK Avatar layout built by 0x82395EC8. Its packed elements have
// no established native conversion; a matching caller can take the original
// nullable creation failure. This does not decode or authorize the layout.
bool isTitleUnavailableAvatarDeclaration(std::span<const uint8_t> records);
// Supported declaration types are exactly the FLOAT2/3/4 and D3DCOLOR constants
// verified in the title constructors. Other encodings stop until established.
std::vector<uint8_t> convertTitleVertexStream(std::span<const uint8_t> bigEndianBytes,
    uint32_t stride, std::span<const D3DVERTEXELEMENT9> declaration);
std::vector<uint16_t> convertTitleTriangleIndices(std::span<const uint8_t> bigEndianBytes);
// Original indexed-UP copies vertices starting at minVertex and emits -min as
// the base. Native user-memory draws consume the equivalent rebased BE16 data.
std::vector<uint16_t> convertTitleImmediateIndices(std::span<const uint8_t> bigEndianBytes,
    uint32_t minVertex, uint32_t vertexCount);
}
