#include "title_vertex.h"
#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

namespace Doritos::Graphics {
namespace {
uint16_t be16(std::span<const uint8_t> data, size_t offset) {
    return uint16_t(data[offset]) << 8 | data[offset + 1];
}
uint32_t be32(std::span<const uint8_t> data, size_t offset) {
    return uint32_t(data[offset]) << 24 | uint32_t(data[offset + 1]) << 16 |
        uint32_t(data[offset + 2]) << 8 | data[offset + 3];
}
uint8_t nativeType(uint32_t encoding) {
    // Distinct from Xbox GPU enum values: do not cast the encoded DWORD to the
    // native D3DDECLTYPE enumeration. These values are from original PPC stores.
    switch (encoding) {
    case 0x002c23a5: return D3DDECLTYPE_FLOAT2;
    case 0x002a23b9: return D3DDECLTYPE_FLOAT3;
    case 0x001a23a6: return D3DDECLTYPE_FLOAT4;
    case 0x00182886: return D3DDECLTYPE_D3DCOLOR;
    default: throw std::runtime_error("Unsupported original Xbox vertex element encoding");
    }
}
uint32_t elementBytes(uint8_t type) {
    switch (type) {
    case D3DDECLTYPE_FLOAT2: return 8;
    case D3DDECLTYPE_FLOAT3: return 12;
    case D3DDECLTYPE_FLOAT4: return 16;
    case D3DDECLTYPE_D3DCOLOR: return 4;
    default: throw std::runtime_error("Native vertex conversion requires a verified original element type");
    }
}
}
bool isTitleUnavailableAvatarDeclaration(std::span<const uint8_t> records) {
    struct Element { uint16_t stream, offset; uint32_t encoding; uint8_t method, usage, index; };
    // Verified original stores at 0x82395EC8, first call LR 0x82396320. Record
    // byte 11 is undefined padding and is neither initialized nor consumed.
    constexpr std::array<Element, 12> expected{{
        {0,0,0x002a23b9,0,0,0}, {0,12,0x002a2190,0,3,0},
        {0,16,0x001a2086,0,1,0}, {0,20,0x001a2286,0,2,0},
        {0,24,0x00182886,0,10,0}, {0,28,0x002c235f,0,5,0},
        {0,32,0x002c235f,0,5,1}, {0,36,0x002c235f,0,5,2},
        {1,0,0x001a23a6,0,5,6}, {1,16,0x001a23a6,0,5,7},
        {1,32,0x001a23a6,0,5,8}, {255,0,UINT32_MAX,0,0,0}
    }};
    if (records.size() != expected.size() * 12) return false;
    for (size_t index = 0; index < expected.size(); ++index) {
        const size_t offset = index * 12;
        const auto& element = expected[index];
        if (be16(records, offset) != element.stream || be16(records, offset + 2) != element.offset ||
            be32(records, offset + 4) != element.encoding || records[offset + 8] != element.method ||
            records[offset + 9] != element.usage || records[offset + 10] != element.index) return false;
    }
    return true;
}
std::vector<D3DVERTEXELEMENT9> decodeTitleDeclaration(std::span<const uint8_t> records) {
    std::vector<D3DVERTEXELEMENT9> result;
    for (size_t offset = 0; offset + 12 <= records.size() && result.size() <= MAXD3DDECLLENGTH; offset += 12) {
        const uint16_t stream = be16(records, offset), elementOffset = be16(records, offset + 2);
        const uint32_t encoding = be32(records, offset + 4);
        if (stream == 255) {
            if (elementOffset || encoding != UINT32_MAX || records[offset + 8] || records[offset + 9] || records[offset + 10])
                throw std::runtime_error("Malformed original Xbox vertex declaration terminator");
            const D3DVERTEXELEMENT9 end = D3DDECL_END();
            result.push_back(end);
            return result;
        }
        if (stream >= 16 || records[offset + 8] != D3DDECLMETHOD_DEFAULT || records[offset + 9] > D3DDECLUSAGE_SAMPLE)
            throw std::runtime_error("Unsupported original Xbox vertex declaration semantics");
        result.push_back({stream, elementOffset, nativeType(encoding), records[offset + 8], records[offset + 9], records[offset + 10]});
    }
    throw std::runtime_error("Original Xbox vertex declaration is missing its bounded terminator");
}
std::vector<uint8_t> convertTitleVertexStream(std::span<const uint8_t> bytes,
    uint32_t stride, std::span<const D3DVERTEXELEMENT9> declaration) {
    if (bytes.empty() || bytes.size() > 128 * 1024 * 1024 || !stride || stride > 2048 || bytes.size() % stride ||
        declaration.empty() || declaration.size() > MAXD3DDECLLENGTH + 1 || declaration.back().Stream != 255)
        throw std::runtime_error("Invalid original title vertex buffer length/stride/declaration");
    std::set<uint32_t> components;
    for (size_t index = 0; index + 1 < declaration.size(); ++index) {
        const auto& element = declaration[index];
        if (element.Stream != 0 || element.Method != D3DDECLMETHOD_DEFAULT)
            throw std::runtime_error("Only verified single-stream title vertex buffers are supported");
        const uint32_t size = elementBytes(element.Type);
        if (element.Offset % 4 || element.Offset > stride || size > stride - element.Offset)
            throw std::runtime_error("Original title vertex element exceeds stride or is unaligned");
        for (uint32_t component = element.Offset; component < element.Offset + size; component += 4)
            if (!components.insert(component).second) throw std::runtime_error("Original title vertex elements overlap");
    }
    std::vector<uint8_t> native(bytes.begin(), bytes.end());
    for (size_t vertex = 0; vertex < bytes.size(); vertex += stride)
        for (const auto component : components) std::reverse(native.begin() + vertex + component, native.begin() + vertex + component + 4);
    return native;
}
std::vector<uint16_t> convertTitleTriangleIndices(std::span<const uint8_t> bytes) {
    // This title's index object count is triangles: 0x823868B8 allocates count*6.
    if (bytes.empty() || bytes.size() > 128 * 1024 * 1024 || bytes.size() % 6)
        throw std::runtime_error("Original title triangle index buffer is not a whole BE16 triangle list");
    std::vector<uint16_t> native(bytes.size() / 2);
    for (size_t index = 0; index < native.size(); ++index) native[index] = be16(bytes, index * 2);
    return native;
}
std::vector<uint16_t> convertTitleImmediateIndices(std::span<const uint8_t> bytes,
    uint32_t minVertex, uint32_t vertexCount) {
    if (bytes.empty() || bytes.size() > 128 * 1024 * 1024 || bytes.size() % 2 || !vertexCount ||
        minVertex > UINT16_MAX || uint64_t(minVertex) + vertexCount > uint64_t(UINT16_MAX) + 1)
        throw std::runtime_error("Invalid original title immediate BE16 index range");
    std::vector<uint16_t> native(bytes.size() / 2);
    for (size_t index = 0; index < native.size(); ++index) {
        const uint32_t value = be16(bytes, index * 2);
        if (value < minVertex || value - minVertex >= vertexCount)
            throw std::runtime_error("Original immediate index lies outside its copied vertex range");
        native[index] = static_cast<uint16_t>(value - minVertex);
    }
    return native;
}
}
