#pragma once
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace Doritos::Graphics {
// The title's 823BE6E8/823BE7C0 uploads read CPU float4 registers, not MMIO.
// This validates the request; the bridge still validates the entire committed
// readable guest span before constructing the byte view below.
inline uint32_t titleFloatConstantSpanBytes(uint32_t address, uint32_t firstRegister, uint32_t registerCount) {
    if (!registerCount || registerCount > 1024 || firstRegister > 1024 || registerCount > 1024 - firstRegister)
        throw std::runtime_error("Original shader register upload exceeds native adapter bounds");
    const uint32_t bytes = registerCount * 16;
    const uint64_t end = uint64_t(address) + bytes;
    if (address < 0x10000 || end > 0x100000000ULL ||
        (address < 0x7fd00000u && end > 0x7fc00000u) ||
        (address < 0x7ff00000u && end > 0x7fe00000u))
        throw std::runtime_error("Original shader constants require a bounded CPU memory span");
    return bytes;
}

// Decode a once-validated span without per-word guest-memory queries. Byte
// reads support unaligned original pointers; bit_cast preserves every float
// bit, including signed zero, infinities, subnormals and NaN payloads.
inline std::vector<float> decodeTitleFloatConstants(std::span<const uint8_t> bytes, uint32_t registerCount) {
    if (!registerCount || registerCount > 1024 || bytes.size() != size_t(registerCount) * 16)
        throw std::runtime_error("Original shader constant byte span does not match its register count");
    std::vector<float> result(size_t(registerCount) * 4);
    for (size_t index = 0; index < result.size(); ++index) {
        const auto* source = bytes.data() + index * 4;
        const uint32_t bits = (uint32_t(source[0]) << 24) | (uint32_t(source[1]) << 16) |
            (uint32_t(source[2]) << 8) | uint32_t(source[3]);
        result[index] = std::bit_cast<float>(bits);
    }
    return result;
}
}
