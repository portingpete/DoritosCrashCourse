#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace Doritos::Native {
// These are observations of actual completed VirtualQuery calls. They never
// authorize memory access or retain a pointer to the queried memory.
struct MemoryQueryRegionKey {
    uint64_t baseAddress = 0, regionSize = 0;
    uint32_t state = 0, protect = 0;
    bool queryReturned = false;
    bool operator==(const MemoryQueryRegionKey&) const = default;
};
struct MemoryQueryRegionObservation {
    MemoryQueryRegionKey key;
    uint32_t requestedAddress = 0, requestedBytes = 0, cursor = 0;
};
struct MemoryQueryRegionTotals {
    uint64_t queries = 0, failedQueries = 0, queryNanoseconds = 0, worstQueryNanoseconds = 0;
    uint32_t firstAddress = 0, firstBytes = 0, firstCursor = 0;
    bool mixedRequests = false, mixedCursors = false;
    void add(const MemoryQueryRegionObservation& observation, uint64_t elapsed) noexcept {
        if (!queries) {
            firstAddress = observation.requestedAddress;
            firstBytes = observation.requestedBytes;
            firstCursor = observation.cursor;
        } else {
            mixedRequests |= firstAddress != observation.requestedAddress || firstBytes != observation.requestedBytes;
            mixedCursors |= firstCursor != observation.cursor;
        }
        ++queries;
        if (!observation.key.queryReturned) ++failedQueries;
        queryNanoseconds += elapsed;
        worstQueryNanoseconds = std::max(worstQueryNanoseconds, elapsed);
    }
};
struct MemoryQueryRegionSite {
    MemoryQueryRegionKey key;
    MemoryQueryRegionTotals totals;
};
struct MemoryQueryRegionBatch {
    static constexpr unsigned siteLimit = 256;
    static constexpr uint64_t queryLimit = 65536;
    std::array<MemoryQueryRegionSite, siteLimit> sites{};
    MemoryQueryRegionTotals overflow;
    unsigned used = 0;
    uint64_t queries = 0, failedQueries = 0, queryNanoseconds = 0, reports = 0;

    bool record(MemoryQueryRegionObservation observation, uint64_t elapsed) noexcept {
        // A failed query has no returned region metadata, even if a caller's
        // output buffer contains stale bytes. It has one explicit identity.
        if (!observation.key.queryReturned) observation.key = {};
        const unsigned start = unsigned(hash(observation.key) & (slots_.size() - 1));
        MemoryQueryRegionTotals* totals = nullptr;
        for (unsigned probe = 0; probe < slots_.size(); ++probe) {
            auto& slot = slots_[(start + probe) & (slots_.size() - 1)];
            if (!slot) {
                if (used < siteLimit) {
                    const unsigned index = used++;
                    sites[index].key = observation.key;
                    slot = uint16_t(index + 1);
                    totals = &sites[index].totals;
                }
                break;
            }
            auto& site = sites[slot - 1];
            if (site.key == observation.key) { totals = &site.totals; break; }
        }
        if (!totals) totals = &overflow;
        totals->add(observation, elapsed);
        ++queries;
        if (!observation.key.queryReturned) ++failedQueries;
        queryNanoseconds += elapsed;
        return queries == queryLimit;
    }
    std::array<const MemoryQueryRegionSite*, siteLimit> ranked() const noexcept {
        std::array<const MemoryQueryRegionSite*, siteLimit> result{};
        for (unsigned index = 0; index < used; ++index) result[index] = &sites[index];
        std::sort(result.begin(), result.begin() + used, [](const auto* left, const auto* right) {
            if (left->totals.queryNanoseconds != right->totals.queryNanoseconds)
                return left->totals.queryNanoseconds > right->totals.queryNanoseconds;
            return left->totals.queries > right->totals.queries;
        });
        return result;
    }
    void clear() noexcept {
        sites = {}; slots_ = {}; overflow = {};
        used = 0; queries = 0; failedQueries = 0; queryNanoseconds = 0;
        // Report numbering is cumulative; all observations are batch-local.
    }
private:
    std::array<uint16_t, 1024> slots_{};
    static uint64_t hash(const MemoryQueryRegionKey& key) noexcept {
        uint64_t value = key.baseAddress;
        value ^= key.regionSize + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
        value ^= (uint64_t(key.state) << 32) | key.protect;
        value ^= uint64_t(key.queryReturned);
        value ^= value >> 30; value *= 0xbf58476d1ce4e5b9ull;
        value ^= value >> 27; value *= 0x94d049bb133111ebull;
        return value ^ (value >> 31);
    }
};
}
