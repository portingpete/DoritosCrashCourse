#include "title_texture.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Doritos::Graphics {
namespace {
constexpr uint64_t maximumBytes = 128ull * 1024 * 1024;
uint32_t read32(std::span<const uint8_t> bytes, size_t offset, bool bigEndian = false) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Truncated title texture field");
    uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index)
        value |= uint32_t(bytes[offset + index]) << ((bigEndian ? 3 - index : index) * 8);
    return value;
}
void dimensions(uint32_t width, uint32_t height) {
    if (!width || !height || width > 8192 || height > 8192)
        throw std::runtime_error("Unsupported title texture dimensions");
}
}
TitleTextureFormat decodeTitleTextureFormat(uint32_t xboxFormat) {
    // sub_8238E448/8238E848 choose these exact format words for stock images.
    // Bit 8 controls tiled allocation, bits 6..7 control fetch byte ordering.
    const uint32_t linear = xboxFormat & ~0x100u;
    D3DFORMAT native;
    uint32_t blockWidth = 1, blockHeight = 1, blockBytes = 4;
    bool depth = false;
    switch (linear) {
    case 0x18280086: native = D3DFMT_A8R8G8B8; break;
    case 0x28280086: native = D3DFMT_X8R8G8B8; break;
    case 0x28280006: native = D3DFMT_X8R8G8B8; break; // Original device RTT create.
    case 0x1a200052: native = D3DFMT_DXT1; blockWidth = blockHeight = 4; blockBytes = 8; break;
    case 0x1a200054: native = D3DFMT_DXT5; blockWidth = blockHeight = 4; blockBytes = 16; break;
    case 0x2d200096: native = D3DFMT_D24S8; depth = true; break;
    default: throw std::runtime_error("Unverified Xbox title texture format");
    }
    return {native, blockWidth, blockHeight, blockBytes, (xboxFormat >> 6) & 3,
        (xboxFormat & 0x100) != 0, depth};
}
TitleTextureRect decodeTitleTextureRect(std::span<const uint8_t> bytes) {
    if (bytes.size() != 16) throw std::runtime_error("Invalid title texture rectangle size");
    TitleTextureRect rect{read32(bytes,0,true),read32(bytes,4,true),read32(bytes,8,true),read32(bytes,12,true)};
    if (rect.left >= rect.right || rect.top >= rect.bottom || rect.right > 8192 || rect.bottom > 8192)
        throw std::runtime_error("Invalid title texture rectangle bounds");
    return rect;
}
TitleLinearTexture convertTitleLinearTexture(std::span<const uint8_t> source,
    uint32_t width, uint32_t height, uint32_t sourcePitch, uint32_t xboxFormat) {
    dimensions(width,height);
    const auto format = decodeTitleTextureFormat(xboxFormat);
    if (format.tiled || format.depth)
        throw std::runtime_error("Native texture upload requires verified linear color data");
    const uint32_t rowPitch = ((width + format.blockWidth - 1) / format.blockWidth) * format.blockBytes;
    const uint32_t rows = (height + format.blockHeight - 1) / format.blockHeight;
    const uint64_t sourceBytes = uint64_t(sourcePitch) * (rows - 1) + rowPitch;
    const uint64_t outputBytes = uint64_t(rowPitch) * rows;
    if (sourcePitch < rowPitch || sourceBytes > source.size() || sourceBytes > maximumBytes || outputBytes > maximumBytes)
        throw std::runtime_error("Title texture rows exceed their source or native limit");
    TitleLinearTexture result{format.native,width,height,rowPitch,std::vector<uint8_t>(size_t(outputBytes))};
    const uint32_t group = format.endian == 1 ? 2 : format.endian == 2 ? 4 : 1;
    if (format.endian == 3 || rowPitch % group || sourcePitch % group)
        throw std::runtime_error("Unsupported title texture fetch byte ordering");
    for (uint32_t row = 0; row < rows; ++row) {
        const auto* input = source.data() + size_t(sourcePitch) * row;
        auto* output = result.bytes.data() + size_t(rowPitch) * row;
        for (uint32_t byte = 0; byte < rowPitch; byte += group)
            for (uint32_t part = 0; part < group; ++part) output[byte + part] = input[byte + group - 1 - part];
        if (format.native == D3DFMT_X8R8G8B8)
            for (uint32_t pixel = 0; pixel < width; ++pixel) output[pixel * 4 + 3] = 255;
    }
    return result;
}
TitleDDS parseTitleDDS(std::span<const uint8_t> source) {
    if (source.size() < 128 || source.size() > maximumBytes || read32(source,0) != 0x20534444 ||
        read32(source,4) != 124 || read32(source,76) != 32)
        throw std::runtime_error("Invalid standard title DDS header");
    const uint32_t width = read32(source,16), height = read32(source,12);
    dimensions(width,height);
    const uint32_t caps2 = read32(source,112);
    if (read32(source,24) > 1 || (caps2 & 0x200000) || (caps2 & ~0xfe00u))
        throw std::runtime_error("Unsupported title DDS volume or resource capabilities");
    const bool cube = (caps2 & 0x200) != 0;
    if ((cube && ((caps2 & 0xfc00) != 0xfc00 || width != height)) || (!cube && caps2))
        throw std::runtime_error("Incomplete or nonsquare title DDS cubemap");
    if (read32(source,80) != 4)
        throw std::runtime_error("Title DDS requires a verified compressed FourCC format");
    D3DFORMAT format;
    uint32_t blockBytes;
    switch (read32(source,84)) {
    case 0x31545844: format = D3DFMT_DXT1; blockBytes = 8; break;
    case 0x35545844: format = D3DFMT_DXT5; blockBytes = 16; break;
    default: throw std::runtime_error("Unsupported title DDS FourCC");
    }
    const uint32_t levels = std::max(1u,read32(source,28));
    uint32_t maximumLevels = 1;
    for (uint32_t extent = std::max(width,height); extent > 1; extent >>= 1) ++maximumLevels;
    if (levels > maximumLevels) throw std::runtime_error("Excessive title DDS mip levels");
    TitleDDS result{format,width,height,levels,cube ? 6u : 1u,{}};
    size_t offset = 128;
    for (uint32_t face = 0; face < result.faces; ++face) {
        uint32_t levelWidth = width, levelHeight = height;
        for (uint32_t level = 0; level < levels; ++level) {
            const uint32_t pitch = ((levelWidth + 3) / 4) * blockBytes;
            const size_t count = size_t(pitch) * ((levelHeight + 3) / 4);
            if (offset > source.size() || count > source.size() - offset)
                throw std::runtime_error("Truncated title DDS face or mip data");
            result.images.push_back({face,level,levelWidth,levelHeight,pitch,offset,count});
            offset += count;
            levelWidth = std::max(1u,levelWidth / 2); levelHeight = std::max(1u,levelHeight / 2);
        }
    }
    if (offset != source.size()) throw std::runtime_error("Trailing title DDS data");
    return result;
}
}
