#pragma once
#include <d3d9.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Doritos::Graphics {
struct TitleTextureFormat {
    D3DFORMAT native;
    uint32_t blockWidth, blockHeight, blockBytes;
    uint32_t endian; // Xbox fetch endian: 0 none, 1 byte pairs, 2 four-byte groups.
    bool tiled, depth;
};
// Only formats established from this title's original create/upload callers.
TitleTextureFormat decodeTitleTextureFormat(uint32_t xboxFormat);
struct TitleTextureRect { uint32_t left, top, right, bottom; };
TitleTextureRect decodeTitleTextureRect(std::span<const uint8_t> bigEndianBytes);
struct TitleLinearTexture {
    D3DFORMAT format;
    uint32_t width, height, rowPitch;
    std::vector<uint8_t> bytes;
};
// Converts an original linear upload into tightly packed native D3D9 rows.
// Tiled GPU storage is rejected; it is not inferred from a CPU image buffer.
TitleLinearTexture convertTitleLinearTexture(std::span<const uint8_t> source,
    uint32_t width, uint32_t height, uint32_t sourcePitch, uint32_t xboxFormat);
struct TitleDDSLevel {
    uint32_t face, level, width, height, rowPitch;
    size_t offset, bytes;
};
struct TitleDDS {
    D3DFORMAT format;
    uint32_t width, height, levels, faces;
    std::vector<TitleDDSLevel> images;
};
// Original package DDS payloads are standard little-endian PC DXT data, not
// Xbox GPU tiles. Returned ranges refer to the caller's complete DDS bytes.
TitleDDS parseTitleDDS(std::span<const uint8_t> source);
}
