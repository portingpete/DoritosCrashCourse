#include "runtime/native/memory_query_regions.h"
#include <cstdio>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace Doritos::Native;
namespace {
size_t checks = 0;
void require(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
using Key = std::tuple<bool, uint64_t, uint64_t, uint32_t, uint32_t>;
Key identity(const MemoryQueryRegionKey& key) {
    if (!key.queryReturned) return {false, 0, 0, 0, 0};
    return {true, key.baseAddress, key.regionSize, key.state, key.protect};
}
struct Expected {
    uint64_t queries = 0, failed = 0, nanoseconds = 0, worst = 0;
};
void add(Expected& expected, uint64_t elapsed) {
    ++expected.queries; expected.nanoseconds += elapsed;
    if (elapsed > expected.worst) expected.worst = elapsed;
}
MemoryQueryRegionObservation sample(uint64_t index) {
    return {{0x100000000ull + index * 4096, 0x20000000ull + index * 8192, 0x1000, 4, true},
        uint32_t(0x10000000 + index * 4096), uint32_t(4 + (index % 7) * 4), uint32_t(0x10000000 + index * 4096)};
}
void verifyObservedIdentity() {
    MemoryQueryRegionBatch batch;
    auto observation = sample(1);
    batch.record(observation, 10);
    observation.requestedAddress += 4; observation.requestedBytes = 28; observation.cursor += 4096;
    batch.record(observation, 20);
    require(batch.used == 1 && batch.sites[0].totals.queries == 2 && batch.sites[0].totals.queryNanoseconds == 30,
        "different requests sharing identical actual region metadata aggregate");
    const auto& totals = batch.sites[0].totals;
    require(totals.firstAddress == sample(1).requestedAddress && totals.firstBytes == sample(1).requestedBytes &&
        totals.firstCursor == sample(1).cursor && totals.mixedRequests && totals.mixedCursors && totals.worstQueryNanoseconds == 20,
        "first requested range and cursor remain observations while variation is explicit");
    auto changed = sample(1);
    changed.key.regionSize += 4096; batch.record(changed, 30);
    changed = sample(1); changed.key.protect = 0x104; batch.record(changed, 40);
    changed = sample(1); changed.key.state = 0x2000; batch.record(changed, 50);
    changed = sample(1); changed.key.baseAddress += 4096; batch.record(changed, 60);
    require(batch.used == 5, "changed returned base, region size, state or protection has distinct identity");
    changed.key.queryReturned = false; batch.record(changed, 70);
    changed = sample(200); changed.key.queryReturned = false; batch.record(changed, 80);
    require(batch.used == 6 && batch.sites[5].key == MemoryQueryRegionKey{} && batch.sites[5].totals.queries == 2 &&
        batch.sites[5].totals.failedQueries == 2 && batch.failedQueries == 2,
        "failed queries use one explicit metadata-invalid identity regardless of stale output bytes");
    changed = sample(2); changed.key.baseAddress = UINT64_MAX - 4095; changed.key.regionSize = UINT64_MAX;
    batch.record(changed, 90);
    require(batch.sites[6].key == changed.key, "returned metadata preserves all 64 bits without range arithmetic");
}
void verifyBoundAndOracle() {
    MemoryQueryRegionBatch batch;
    std::map<Key, Expected> expected;
    Expected overflow;
    uint64_t expectedQueries = 0, expectedFailedQueries = 0, expectedNanoseconds = 0;
    // Seed known first identities, then revisit both tracked and untracked
    // identities in a deterministic shuffled stream. The oracle is a normal
    // value map, independent of the production bounded hash/probe mechanism.
    const auto record = [&](MemoryQueryRegionObservation observation, uint64_t elapsed) {
        const Key key = identity(observation.key);
        auto found = expected.find(key);
        if (found == expected.end() && expected.size() < 256) found = expected.emplace(key, Expected{}).first;
        Expected& destination = found == expected.end() ? overflow : found->second;
        add(destination, elapsed);
        if (!observation.key.queryReturned) { ++destination.failed; ++expectedFailedQueries; }
        ++expectedQueries; expectedNanoseconds += elapsed;
        const bool ready = batch.record(observation, elapsed);
        require(ready == (expectedQueries == 65536), "report cadence counts actual completed queries exactly");
    };
    for (unsigned index = 0; index < 256; ++index) record(sample(index), index + 1);
    for (uint64_t index = 256; index < 65536; ++index) {
        auto observation = sample((index * 191 + 17) % 340);
        if (index % 997 == 0) observation.key.queryReturned = false;
        observation.cursor += uint32_t(index & 127);
        observation.requestedBytes += uint32_t(index & 31);
        record(observation, 1 + (index * 31) % 10000);
    }
    require(batch.used == 256 && batch.queries == expectedQueries && batch.failedQueries == expectedFailedQueries &&
        batch.queryNanoseconds == expectedNanoseconds,
        "fixed identity bound retains exact all-query totals");
    require(batch.overflow.queries == overflow.queries && batch.overflow.failedQueries == overflow.failed &&
        batch.overflow.queryNanoseconds == overflow.nanoseconds &&
        batch.overflow.worstQueryNanoseconds == overflow.worst,
        "overflow preserves repeated untracked query counts, elapsed sum and worst query");
    uint64_t countedQueries = batch.overflow.queries, countedNanoseconds = batch.overflow.queryNanoseconds;
    for (unsigned index = 0; index < batch.used; ++index) {
        const auto& actual = batch.sites[index];
        const auto found = expected.find(identity(actual.key));
        require(found != expected.end(), "every stored identity belongs to the independent oracle");
        require(actual.totals.queries == found->second.queries && actual.totals.failedQueries == found->second.failed &&
            actual.totals.queryNanoseconds == found->second.nanoseconds &&
            actual.totals.worstQueryNanoseconds == found->second.worst, "per-identity measurements match independent accounting");
        countedQueries += actual.totals.queries; countedNanoseconds += actual.totals.queryNanoseconds;
    }
    require(countedQueries == expectedQueries && countedNanoseconds == expectedNanoseconds,
        "tracked identities and overflow sum to the full actual-query batch without double counting");
    std::vector<std::pair<uint64_t, uint64_t>> expectedRanking;
    for (const auto& [key, value] : expected) expectedRanking.emplace_back(value.nanoseconds, value.queries);
    std::sort(expectedRanking.begin(), expectedRanking.end(), std::greater<>());
    const auto ranked = batch.ranked();
    for (unsigned index = 0; index < 8; ++index)
        require(std::pair{ranked[index]->totals.queryNanoseconds, ranked[index]->totals.queries} == expectedRanking[index],
            "top eight correspond to independent descending query-time and count ranking");

    batch.reports = 7;
    batch.clear();
    require(batch.used == 0 && batch.queries == 0 && batch.failedQueries == 0 && batch.queryNanoseconds == 0 &&
        batch.overflow.queries == 0 && batch.reports == 7,
        "new batch resets observations and overflow while preserving report numbering");
    auto reused = sample(339);
    batch.record(reused, 123);
    require(batch.used == 1 && batch.sites[0].key == reused.key && batch.sites[0].totals.queries == 1 &&
        batch.sites[0].totals.queryNanoseconds == 123 && !batch.sites[0].totals.mixedRequests && batch.overflow.queries == 0,
        "previously overflowed identity can be tracked fresh without stale slots or totals");
    MemoryQueryRegionBatch independent;
    independent.record(sample(7), 456);
    require(batch.queries == 1 && batch.queryNanoseconds == 123 && independent.queries == 1 && independent.queryNanoseconds == 456,
        "independent recorder instances share no mutable accounting state");
}
}
int main() {
    try {
        verifyObservedIdentity(); verifyBoundAndOracle();
        std::printf("Native memory query region contract passed: %zu checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native memory query region contract failed: %s\n", error.what());
        return 1;
    }
}
