#pragma once
#include <cstdint>

namespace Doritos::Native {
// Callers validate the address/range and hold the reservation lock. Keeping
// both insertions here prevents a failed map allocation from leaving an
// unreturned reservation behind. The maps hold uint32_t keys/values; erasing
// the newly inserted node does not allocate or compare keys.
template<class AllocationMap, class PageMap>
bool recordGuestReservation(AllocationMap& allocations, PageMap& pages,
    uint32_t address, uint32_t bytes, uint32_t pageSize, uint32_t* cursor = nullptr) {
    const auto [allocation, inserted] = allocations.try_emplace(address, bytes);
    if (!inserted) return false;
    bool pageInserted;
    try {
        pageInserted = pages.try_emplace(address, pageSize).second;
    } catch (...) {
        allocations.erase(allocation);
        throw;
    }
    if (!pageInserted) {
        allocations.erase(allocation);
        return false;
    }
    // Only Memory::reserve supplies its cursor, after checking the sum.
    if (cursor) *cursor = address + bytes;
    return true;
}
}
