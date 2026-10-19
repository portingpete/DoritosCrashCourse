#pragma once
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace Doritos::Native {
constexpr unsigned residentPageValidationLimit = 128;

namespace Detail {
template<class Query>
bool residentReadWritePageEntries(PSAPI_WORKING_SET_EX_INFORMATION* entries, unsigned pages, Query& query) {
    struct LastErrorRestore { DWORD value = GetLastError(); ~LastErrorRestore() { SetLastError(value); } } lastErrorRestore;
    if (!query(entries, pages)) return false;
    for (unsigned index = 0; index < pages; ++index) {
        const auto attributes = entries[index].VirtualAttributes;
        // Other bitfields are meaningful only when Valid is set.
        if (!attributes.Valid || attributes.Win32Protection != PAGE_READWRITE ||
            attributes.Bad || attributes.LargePage || attributes.Shared) return false;
    }
    return true;
}
template<unsigned Capacity, class Query>
bool residentReadWritePageBatch(uintptr_t nativeBase, uint64_t first, unsigned pages,
    uint32_t pageSize, Query& query) {
    std::array<PSAPI_WORKING_SET_EX_INFORMATION, Capacity> entries{};
    for (unsigned index = 0; index < pages; ++index)
        entries[index].VirtualAddress = reinterpret_cast<PVOID>(nativeBase + first + uint64_t(index) * pageSize);
    return residentReadWritePageEntries(entries.data(), pages, query);
}
}

// Positive proof only for the runtime's ordinary private VirtualAlloc reserve
// and commits. Resident private pages imply committed backing in that mapping;
// no AWE, mapped views or large-page allocations are used by the runtime.
// False means unproven: the caller must run its unchanged VirtualQuery policy.
// Each invocation queries every intersecting page freshly, without accessing
// target bytes. The observations expire on return and are never cached.
template<class Query>
bool residentReadWritePages(const uint8_t* base, uint32_t address, uint32_t bytes, uint32_t pageSize, Query&& query) {
    const uint64_t end = uint64_t(address) + bytes;
    const uintptr_t nativeBase = reinterpret_cast<uintptr_t>(base);
    if (!base || address < 0x10000 || !bytes || end > 0x100000000ull || !pageSize ||
        (pageSize & (pageSize - 1)) || (nativeBase & (pageSize - 1)) ||
        nativeBase > (std::numeric_limits<uintptr_t>::max)() - (end - 1)) return false;
    const uint64_t first = uint64_t(address) & ~(uint64_t(pageSize) - 1);
    const uint64_t last = (end - 1) & ~(uint64_t(pageSize) - 1);
    const uint64_t pages = (last - first) / pageSize + 1;
    if (pages > residentPageValidationLimit) return false;

    // Keep the small stack buffer for the far more frequent scalar checks.
    // A larger bounded batch also covers the original renderer owner/prefix,
    // without a VirtualQuery scan to the end of a large committed region.
    if (pages <= 16)
        return Detail::residentReadWritePageBatch<16>(nativeBase,first,unsigned(pages),pageSize,query);
    return Detail::residentReadWritePageBatch<residentPageValidationLimit>(nativeBase,first,unsigned(pages),pageSize,query);
}

// One fresh query for two immediately consumed spans. Pages shared by the
// spans appear once; no observation survives this invocation. Unproven pairs
// must use the same scalar VirtualQuery fallback as ordinary valid() calls.
template<class Query>
bool residentReadWritePagePair(const uint8_t* base,
    uint32_t firstAddress, uint32_t firstBytes, uint32_t secondAddress, uint32_t secondBytes,
    uint32_t pageSize, Query&& query) {
    const uintptr_t nativeBase = reinterpret_cast<uintptr_t>(base);
    const uint64_t firstEnd = uint64_t(firstAddress) + firstBytes;
    const uint64_t secondEnd = uint64_t(secondAddress) + secondBytes;
    if (!base || firstAddress < 0x10000 || secondAddress < 0x10000 || !firstBytes || !secondBytes ||
        firstEnd > 0x100000000ull || secondEnd > 0x100000000ull || !pageSize ||
        (pageSize & (pageSize - 1)) || (nativeBase & (pageSize - 1)) ||
        nativeBase > (std::numeric_limits<uintptr_t>::max)() - (std::max(firstEnd, secondEnd) - 1)) return false;
    constexpr unsigned capacity = 16;
    std::array<PSAPI_WORKING_SET_EX_INFORMATION, capacity> entries{};
    unsigned pages = 0;
    const auto append = [&](uint32_t address, uint64_t end) {
        const uint64_t first = uint64_t(address) & ~(uint64_t(pageSize) - 1);
        const uint64_t last = (end - 1) & ~(uint64_t(pageSize) - 1);
        if ((last - first) / pageSize + 1 > capacity) return false;
        for (uint64_t page = first; page <= last; page += pageSize) {
            const auto pointer = reinterpret_cast<PVOID>(nativeBase + page);
            bool present = false;
            for (unsigned index = 0; index < pages; ++index) present |= entries[index].VirtualAddress == pointer;
            if (present) continue;
            if (pages == capacity) return false;
            entries[pages++].VirtualAddress = pointer;
        }
        return true;
    };
    if (!append(firstAddress, firstEnd) || !append(secondAddress, secondEnd)) return false;
    return Detail::residentReadWritePageEntries(entries.data(), pages, query);
}
}
