#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace Doritos::Graphics {
// These helpers operate only within a span validated by the calling bridge.
// They preserve integer/float bits and never retain guest memory between calls.
inline std::span<const uint8_t> titleGuestOwnedRange(std::span<const uint8_t> owner,
    uint32_t ownerAddress, uint64_t address, std::size_t count) {
    constexpr uint64_t addressSpace = uint64_t(1) << 32;
    if (ownerAddress < 0x10000 || owner.size() > addressSpace - ownerAddress ||
        !count || address < ownerAddress || address >= addressSpace)
        throw std::runtime_error("Title guest range exceeds its validated owner");
    const uint64_t offset = address - ownerAddress;
    if (offset > owner.size() || count > owner.size() - offset)
        throw std::runtime_error("Title guest range exceeds its validated owner");
    return owner.subspan(static_cast<std::size_t>(offset),count);
}
inline uint32_t titleGuestWord(std::span<const uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Title guest word exceeds its validated span");
    return uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset + 1]) << 16 |
        uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
}
inline void titleGuestWriteWord(std::span<uint8_t> bytes, std::size_t offset, uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Title guest word exceeds its validated span");
    bytes[offset] = uint8_t(value >> 24);
    bytes[offset + 1] = uint8_t(value >> 16);
    bytes[offset + 2] = uint8_t(value >> 8);
    bytes[offset + 3] = uint8_t(value);
}
}
