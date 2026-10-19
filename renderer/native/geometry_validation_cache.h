#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace Doritos::Graphics {
// A proof belongs to exact native buffer contents and a complete draw range.
// Producer fields hold all Cal3D or RVMBatch geometry parameters, never live
// shader bindings, matrix palettes or original CPU ownership descriptors.
struct GeometryValidationKey {
    uint32_t vertexHandle = 0, indexHandle = 0;
    uint64_t vertexRevision = 0, indexRevision = 0;
    uint32_t topology = 0, baseVertex = 0, vertexCount = 0, startIndex = 0, triangles = 0;
    uint32_t producer = 0; // 0 = ordinary, 1 = Cal3D, 2 = RVMBatch.
    std::array<uint32_t,7> geometry{};
    bool operator==(const GeometryValidationKey&) const = default;
};
template<size_t Capacity = 256> class GeometryValidationCache {
    static_assert(Capacity > 0);
    struct Entry { GeometryValidationKey key; bool valid = false; };
    std::array<Entry,Capacity> entries_{};
    static size_t slot(const GeometryValidationKey& key) {
        uint64_t hash = 14695981039346656037ull;
        const auto mix = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
        mix(key.vertexHandle); mix(key.indexHandle); mix(key.vertexRevision); mix(key.indexRevision);
        mix(key.topology); mix(key.baseVertex); mix(key.vertexCount); mix(key.startIndex); mix(key.triangles); mix(key.producer);
        for (const auto value : key.geometry) mix(value);
        hash ^= hash >> 32;
        return static_cast<size_t>(hash % Capacity);
    }
public:
    template<class Validator> void validate(const GeometryValidationKey& key, Validator&& validator) {
        auto& entry = entries_[slot(key)];
        if (entry.valid && entry.key == key) return;
        validator(); // A rejected draw never becomes a reusable proof.
        entry = {key,true};
    }
};
}
