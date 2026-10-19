#pragma once
#include <cstdint>
#include <iterator>

namespace Doritos::Native {
// Read the two ownership maps in address order without allocating or changing
// them. Callers hold their reservation lock and publish a chosen gap only
// after this returns. A decommitted reservation remains an occupied range.
template<class AllocationMap, class FixedMap>
uint32_t findGuestReservationGap(const AllocationMap& allocations, const FixedMap& fixed,
    uint64_t bytes, uint32_t alignment, uint64_t begin, uint64_t endExclusive) {
    constexpr uint64_t addressSpace = 0x100000000ull;
    if (!bytes || !alignment || (alignment & (alignment - 1)) || begin < 0x10000 ||
        endExclusive > addressSpace || begin >= endExclusive || bytes > endExclusive - begin)
        return 0;
    const auto align = [alignment](uint64_t address) {
        return (address + alignment - 1) & ~(uint64_t(alignment) - 1);
    };
    const auto first = [begin](const auto& ranges) {
        auto entry = ranges.upper_bound(static_cast<uint32_t>(begin));
        if (entry != ranges.begin()) {
            auto previous = std::prev(entry);
            if (uint64_t(previous->first) + previous->second > begin) entry = previous;
        }
        return entry;
    };
    auto allocation = first(allocations);
    auto mapping = first(fixed);
    uint64_t address = align(begin);
    while (allocation != allocations.end() || mapping != fixed.end()) {
        const bool useAllocation = mapping == fixed.end() ||
            (allocation != allocations.end() && allocation->first <= mapping->first);
        uint64_t occupied, length;
        if (useAllocation) {
            occupied = allocation->first; length = allocation->second; ++allocation;
        } else {
            occupied = mapping->first; length = mapping->second; ++mapping;
        }
        if (address + bytes > endExclusive) return 0;
        const uint64_t end = occupied + length;
        if (end <= address) continue;
        if (address + bytes <= occupied) return static_cast<uint32_t>(address);
        address = align(end);
    }
    return address + bytes <= endExclusive ? static_cast<uint32_t>(address) : 0;
}
}
