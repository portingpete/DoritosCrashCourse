#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include "runtime.h"
#include "kernel_objects.h"
#include "reservation_transaction.h"
#include "reservation_gap.h"
#include "memory_query_regions.h"
#include "resident_page_validation.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace Doritos::Native {
Memory* memory = nullptr;
thread_local PPCContext* currentContext = nullptr;
static MmioReadHandler mmioReadHandler = nullptr;
static MmioWriteHandler mmioWriteHandler = nullptr;
void setNativeMmioHandlers(MmioReadHandler read, MmioWriteHandler write) {
    mmioReadHandler = read;
    mmioWriteHandler = write;
}
static constexpr uint64_t kAddressSpace = 0x100000000ull;
namespace {
thread_local MemoryQueryCounters memoryQueries;
thread_local CriticalMemoryQueryCounters criticalMemoryQueries;
thread_local MemoryQueryRegionBatch memoryQueryRegions;
struct MemoryQueryApiBatch {
    uint64_t workingSetCalls = 0, virtualQueryCalls = 0, workingSetNanoseconds = 0, virtualQueryNanoseconds = 0, reports = 0;
};
thread_local MemoryQueryApiBatch memoryQueryApis;
uint64_t memoryQueryClock() noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint32_t validationPageSize() noexcept {
    // Host granularity is immutable, not a cached guest validity result.
    static const uint32_t size = [] {
        const DWORD lastError = GetLastError();
        SYSTEM_INFO info{}; GetSystemInfo(&info);
        SetLastError(lastError);
        return uint32_t(info.dwPageSize);
    }();
    return size;
}
void recordMemoryQueryApi(bool workingSet, uint64_t elapsed) noexcept {
    auto& batch = memoryQueryApis;
    if (workingSet) { ++batch.workingSetCalls; batch.workingSetNanoseconds += elapsed; }
    else { ++batch.virtualQueryCalls; batch.virtualQueryNanoseconds += elapsed; }
    if (batch.workingSetCalls + batch.virtualQueryCalls != 65536) return;
    const DWORD lastError = GetLastError();
    std::fprintf(stderr, "[MEMORY QUERY APIS] thread=%lu batch=%llu queries=65536 working_set_queries=%llu virtual_queries=%llu "
        "working_set_query_ms=%.3f virtual_query_ms=%.3f scope=current_thread_actual_api_query_batch "
        "query_subset_of_whole_queries=true not_additive_to_other_query_reports=true "
        "region_reports_cover_virtual_query_only=true frame_interval_measured=false "
        "query_wall_includes_possible_preemption=true recording_overhead_measured=false\n",
        GetCurrentThreadId(), ++batch.reports, batch.workingSetCalls, batch.virtualQueryCalls,
        double(batch.workingSetNanoseconds) / 1e6, double(batch.virtualQueryNanoseconds) / 1e6);
    batch.workingSetCalls = batch.virtualQueryCalls = batch.workingSetNanoseconds = batch.virtualQueryNanoseconds = 0;
    SetLastError(lastError);
}
void reportMemoryQueryRegions() noexcept {
    // Logging observes completed queries only and must not replace the last
    // error produced by the original query. No guest memory is touched here.
    struct LastErrorRestore { DWORD value = GetLastError(); ~LastErrorRestore() { SetLastError(value); } } lastErrorRestore;
    auto& batch = memoryQueryRegions;
    const auto ranked = batch.ranked();
    const DWORD thread = GetCurrentThreadId();
    const uint64_t report = ++batch.reports;
    std::fprintf(stderr, "[MEMORY QUERY REGIONS] thread=%lu batch=%llu queries=%llu failed_queries=%llu query_ms=%.3f sites=%u "
        "site_limit=256 overflow_queries=%llu overflow_failed_queries=%llu overflow_query_ms=%.3f top_limit=8 sort=query_nanoseconds "
        "scope=current_thread_actual_query_batch query_subset_of_whole_queries=true "
        "not_additive_to_other_query_reports=true frame_interval_measured=false "
        "query_wall_includes_possible_preemption=true recording_overhead_measured=false "
        "region_observations_not_lifetime_proofs=true\n",
        thread, report, batch.queries, batch.failedQueries, double(batch.queryNanoseconds) / 1e6, batch.used,
        batch.overflow.queries, batch.overflow.failedQueries, double(batch.overflow.queryNanoseconds) / 1e6);
    const auto print = [&](const MemoryQueryRegionKey& key, const MemoryQueryRegionTotals& totals, bool overflow) {
        std::fprintf(stderr, "[MEMORY QUERY REGION] thread=%lu batch=%llu query_returned_known=%u query_returned=%u "
            "region_metadata_valid=%u base_address=%016llX region_size=%llu state=%08X protect=%08X "
            "queries=%llu failed_queries=%llu query_ms=%.3f average_query_us=%.3f worst_query_us=%.3f "
            "first_requested_address=%08X first_requested_bytes=%u mixed_requests=%u "
            "first_cursor=%08X mixed_cursors=%u overflow_bucket=%u "
            "scope=current_thread_actual_query_batch query_subset_of_whole_queries=true "
            "not_additive_to_other_query_reports=true frame_interval_measured=false "
            "region_observations_not_lifetime_proofs=true\n",
            thread, report, unsigned(!overflow), unsigned(!overflow && key.queryReturned), unsigned(!overflow && key.queryReturned),
            key.baseAddress, key.regionSize, key.state, key.protect, totals.queries, totals.failedQueries,
            double(totals.queryNanoseconds) / 1e6, double(totals.queryNanoseconds) / double(totals.queries) / 1e3,
            double(totals.worstQueryNanoseconds) / 1e3, totals.firstAddress, totals.firstBytes,
            unsigned(totals.mixedRequests), totals.firstCursor, unsigned(totals.mixedCursors), unsigned(overflow));
    };
    for (unsigned index = 0; index < std::min(batch.used, 8u); ++index)
        print(ranked[index]->key, ranked[index]->totals, false);
    if (batch.overflow.queries) print({}, batch.overflow, true);
    batch.clear();
}
}
bool memoryQueryTimingEnabled() noexcept {
    static const bool enabled = [] {
        const DWORD lastError = GetLastError();
        wchar_t value[2]{};
        const bool enabled = GetEnvironmentVariableW(L"DORITOS_MEMORY_TIMING",value,2) == 1 && value[0] == L'1';
        SetLastError(lastError);
        return enabled;
    }();
    return enabled;
}
MemoryQueryCounters memoryQueryCounters() noexcept { return memoryQueries; }
CriticalMemoryQueryCounters criticalMemoryQueryCounters() noexcept { return criticalMemoryQueries; }
void recordCriticalMemoryQueries(const MemoryQueryCounters& before) noexcept {
    ++criticalMemoryQueries.calls;
    criticalMemoryQueries.queries.validations += memoryQueries.validations - before.validations;
    criticalMemoryQueries.queries.queries += memoryQueries.queries - before.queries;
    criticalMemoryQueries.queries.queryNanoseconds += memoryQueries.queryNanoseconds - before.queryNanoseconds;
    criticalMemoryQueries.queries.validationNanoseconds += memoryQueries.validationNanoseconds - before.validationNanoseconds;
}

void reportOriginalGameState() {
    static const bool enabled = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"DORITOS_GAME_STATE", value, 2) == 1 && value[0] == L'1';
    }();
    if (!enabled || !memory) return;
    const auto pointer = [](uint32_t global) {
        return memory->valid(global, 4) ? memory->read32(global) : 0;
    };
    // Original setters/getters use these exact fields: NETMAN82245A60,
    // LevelConf821A4048/821A3F50, and LaunchGame82227FA0. A readable pointer
    // is diagnostic context, not proof of a playable or correct game state.
    const uint32_t manager = pointer(0x82baa908), config = pointer(0x82b55084), game = pointer(0x82b5caa0);
    const bool managerReadable = manager && memory->valid(manager, 180);
    const bool configReadable = config && memory->valid(config, 68);
    const bool gameReadable = game && memory->valid(game, 4);
    const uint32_t unknown = UINT32_MAX;
    std::fprintf(stderr,
        "[ORIGINAL GAME STATE] netman=%08X readable=%u vtable=%08X mode=%u state=%u levelconf=%08X readable=%u track=%u theme=%u game=%08X readable=%u vtable=%08X\n",
        manager, unsigned(managerReadable), managerReadable ? memory->read32(manager) : 0,
        managerReadable ? memory->read32(manager + 168) : unknown,
        managerReadable ? memory->read32(manager + 176) : unknown,
        config, unsigned(configReadable), configReadable ? memory->read32(config + 52) : unknown,
        configReadable ? memory->read32(config + 64) : unknown,
        game, unsigned(gameReadable), gameReadable ? memory->read32(game) : 0);
    std::fflush(stderr);
}

[[noreturn]] void fail(const char* reason) {
    fprintf(stderr, "[NATIVE STOP] %s\n", reason);
    if (currentContext) printContext(*currentContext);
    throw NativeFailure(reason);
}
void printContext(const PPCContext& ctx) {
    fprintf(stderr, "last=0x%08X lr=0x%08X ctr=0x%08X sp=0x%08X r3=0x%08X\n",
        uint32_t(ctx.lastFunction), uint32_t(ctx.lr), uint32_t(ctx.ctr.u32), ctx.r1.u32, ctx.r3.u32);
    fprintf(stderr, "args r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X\n",
        ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
    fprintf(stderr, "saved r14=%08X r15=%08X r16=%08X r17=%08X r18=%08X r19=%08X r20=%08X r21=%08X r22=%08X\n",
        ctx.r14.u32, ctx.r15.u32, ctx.r16.u32, ctx.r17.u32, ctx.r18.u32, ctx.r19.u32, ctx.r20.u32, ctx.r21.u32, ctx.r22.u32);
    fprintf(stderr, "saved r23=%08X r24=%08X r25=%08X r26=%08X r27=%08X r28=%08X r29=%08X r30=%08X r31=%08X\n",
        ctx.r23.u32, ctx.r24.u32, ctx.r25.u32, ctx.r26.u32, ctx.r27.u32, ctx.r28.u32, ctx.r29.u32, ctx.r30.u32, ctx.r31.u32);
    fprintf(stderr, "float args f1=%.17g f2=%.17g f3=%.17g\n", ctx.f1.f64, ctx.f2.f64, ctx.f3.f64);
    if (memory && uint64_t(ctx.r1.u32) + 120 <= 0x100000000ull && memory->valid(ctx.r1.u32 + 84, 36)) {
        fprintf(stderr, "stack args +84=%08X +92=%08X +100=%08X +108=%08X +116=%08X\n",
            memory->read32(ctx.r1.u32 + 84), memory->read32(ctx.r1.u32 + 92), memory->read32(ctx.r1.u32 + 100),
            memory->read32(ctx.r1.u32 + 108), memory->read32(ctx.r1.u32 + 116));
    }
    fprintf(stderr, "recent compiled functions:");
    for (unsigned i = 0; i < 64; ++i) fprintf(stderr, " %08X", ctx.trace[(ctx.traceIndex + i) & 63]);
    fprintf(stderr, "\n");
}
Memory::Memory(const LoadedXex& image) {
    const uint64_t mappingBytes = (PPC_CODE_SIZE * 2 + 65535) & ~65535ull;
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kAddressSpace + mappingBytes, MEM_RESERVE, PAGE_NOACCESS));
    if (!base_) fail("Unable to reserve guest address space");
    try {
        if (image.image_base != PPC_IMAGE_BASE || image.image.size() != PPC_IMAGE_SIZE)
            fail("Decoded image layout differs from compiled translation");
        if (image.mapped_image_size < image.image.size()) fail("Invalid mapped image size");
        commit(image.image_base, image.mapped_image_size);
        memcpy(base_ + image.image_base, image.image.data(), image.image.size());
        fixedMappings_[image.image_base] = image.mapped_image_size;
        if (!VirtualAlloc(base_ + kAddressSpace, mappingBytes, MEM_COMMIT, PAGE_READWRITE))
            fail("Unable to commit host function mapping table");
        unsigned mappings = 0;
        for (auto* mapping = PPCFuncMappings; mapping->host; ++mapping) {
            if (mapping->guest < PPC_CODE_BASE || mapping->guest >= PPC_CODE_BASE + PPC_CODE_SIZE || (mapping->guest & 3))
                fail("Generated function mapping outside code range");
            PPC_LOOKUP_FUNC(base_, uint32_t(mapping->guest)) = mapping->host;
            ++mappings;
        }
        fprintf(stderr, "[AOT] %u native function mappings; image %zu bytes\n", mappings, image.image.size());
        constexpr uint32_t kHeader = 0x81000000;
        commit(kHeader, uint32_t(image.header.size()));
        memcpy(base_ + kHeader, image.header.data(), image.header.size());
        fixedMappings_[kHeader] = uint32_t(image.header.size());
        initializeDataImports(image);
    } catch (...) {
        VirtualFree(base_, 0, MEM_RELEASE);
        base_ = nullptr;
        throw;
    }
}
Memory::~Memory() {
    shutdownKernelObjects();
    if (timestampThread_.joinable()) { timestampThread_.request_stop(); timestampThread_.join(); }
    if (base_) VirtualFree(base_, 0, MEM_RELEASE);
}
void Memory::commit(uint32_t address, uint32_t size) {
    std::lock_guard guard(mutex_);
    if (!size || uint64_t(address) + size > kAddressSpace) fail("Invalid guest memory commit range");
    if (!VirtualAlloc(base_ + address, size, MEM_COMMIT, PAGE_READWRITE)) fail("Guest memory commit failed");
}
uint32_t Memory::allocate(uint32_t size, uint32_t alignment) {
    std::lock_guard guard(mutex_);
    uint32_t address = reserve(size, alignment);
    commit(address, allocations_.at(address));
    return address;
}
uint32_t Memory::reserve(uint32_t size, uint32_t alignment, uint32_t pageSize) {
    std::lock_guard guard(mutex_);
    if (!size || !alignment || (alignment & (alignment - 1))) fail("Invalid guest allocation size/alignment");
    alignment = std::max(alignment, 4096u);
    const uint64_t bytes = (uint64_t(size) + alignment - 1) & ~(uint64_t(alignment) - 1);
    constexpr uint32_t heapBegin = 0x10000000;
    // Retain the original strict end limit, including its final-page guard.
    constexpr uint64_t heapEndExclusive = 0x5fffffffull;
    uint32_t address = findGuestReservationGap(allocations_, fixedMappings_, bytes, alignment,
        nextAllocation_, heapEndExclusive);
    if (!address) address = findGuestReservationGap(allocations_, fixedMappings_, bytes, alignment,
        heapBegin, heapEndExclusive);
    if (!address) {
        uint64_t liveBytes = 0, heapBytes = 0, heapReservations = 0;
        for (const auto& [base, length] : allocations_) {
            liveBytes += length;
            const uint64_t end = uint64_t(base) + length;
            if (base >= heapEndExclusive || end <= heapBegin) continue;
            heapBytes += std::min(end,heapEndExclusive) - std::max(uint64_t(base),uint64_t(heapBegin));
            ++heapReservations;
        }
        std::fprintf(stderr,"[MEMORY ALLOCATION FAILURE] requested=%u rounded_bytes=%llu alignment=%u page=%u cursor=%08X "
            "live_heap_reservations=%llu live_heap_reserved_bytes=%llu live_guest_reservations=%zu live_guest_reserved_bytes=%llu "
            "reservations_created=%llu reservations_released=%llu cumulative_reserved_bytes=%llu "
            "scope=logical_owned_reservations committed_bytes_measured=false\n",
            size,bytes,alignment,pageSize,nextAllocation_,heapReservations,heapBytes,allocations_.size(),liveBytes,
            reservationsCreated_,reservationsReleased_,cumulativeReservedBytes_);
        fail("Native guest heap exhausted");
    }
    if (!recordGuestReservation(allocations_, allocationPages_, address, uint32_t(bytes), pageSize, &nextAllocation_))
        fail("Duplicate native guest reservation bookkeeping");
    ++reservationsCreated_; cumulativeReservedBytes_ += bytes;
    return address;
}
bool Memory::reservationContains(uint32_t address, uint32_t bytes) const {
    std::lock_guard guard(mutex_);
    auto found = allocations_.upper_bound(address);
    if (found == allocations_.begin()) return false;
    --found;
    return bytes && uint64_t(address) >= found->first && uint64_t(address) + bytes <= uint64_t(found->first) + found->second;
}
uint32_t Memory::reserveRange(uint32_t size, uint32_t alignment, uint32_t pageSize, uint32_t begin, uint64_t endExclusive) {
    std::lock_guard guard(mutex_);
    if (!size || alignment < 4096 || (alignment & (alignment - 1)) ||
        pageSize < 4096 || (pageSize & (pageSize - 1)) || begin < 0x10000 ||
        endExclusive > kAddressSpace || endExclusive <= begin)
        fail("Invalid bounded native reservation");
    const uint64_t bytes = (uint64_t(size) + pageSize - 1) & ~(uint64_t(pageSize) - 1);
    if (bytes > UINT32_MAX) return 0;
    alignment = std::max(alignment, pageSize);
    auto align = [&](uint64_t address) { return (address + alignment - 1) & ~(uint64_t(alignment) - 1); };
    uint64_t address = align(begin);
    auto occupied = allocations_;
    occupied.insert(fixedMappings_.begin(), fixedMappings_.end());
    for (const auto& [existing, length] : occupied) {
        const uint64_t end = uint64_t(existing) + length;
        if (end <= address) continue;
        if (address + bytes <= existing) break;
        address = align(end);
        if (address + bytes > endExclusive) return 0;
    }
    if (address + bytes > endExclusive) return 0;
    if (!recordGuestReservation(allocations_, allocationPages_, uint32_t(address), uint32_t(bytes), pageSize))
        fail("Duplicate native guest reservation bookkeeping");
    ++reservationsCreated_; cumulativeReservedBytes_ += bytes;
    return uint32_t(address);
}
std::map<uint32_t, uint32_t> Memory::reservationSnapshot() const {
    std::lock_guard guard(mutex_);
    return allocations_;
}
uint32_t Memory::allocationSize(uint32_t address) const {
    std::lock_guard guard(mutex_);
    auto found = allocations_.find(address);
    return found == allocations_.end() ? 0 : found->second;
}
uint32_t Memory::reservationPageSize(uint32_t address) const {
    std::lock_guard guard(mutex_);
    auto found = allocations_.upper_bound(address);
    if (found == allocations_.begin()) return 0;
    --found;
    if (uint64_t(address) >= uint64_t(found->first) + found->second) return 0;
    return allocationPages_.at(found->first);
}
bool Memory::decommit(uint32_t address, uint32_t size) {
    std::lock_guard guard(mutex_);
    auto found = allocations_.upper_bound(address);
    if (found == allocations_.begin()) return false;
    --found;
    const uint32_t page = allocationPages_.at(found->first);
    if (!size || (address & (page - 1)) || (size & (page - 1)) ||
        uint64_t(address) < found->first || uint64_t(address) + size > uint64_t(found->first) + found->second)
        return false;
    // Windows discards physical storage (also for mixed committed/reserved
    // ranges). Keeping these maps intact preserves the guest's CRT heap.
    if (!VirtualFree(base_ + address, size, MEM_DECOMMIT)) fail("Native guest memory decommit failed");
    return true;
}
bool Memory::release(uint32_t address) {
    std::lock_guard guard(mutex_);
    auto it = allocations_.find(address);
    if (it == allocations_.end()) return false;
    if (!VirtualFree(base_ + address, it->second, MEM_DECOMMIT)) fail("Native guest memory decommit failed");
    ++reservationsReleased_;
    allocations_.erase(it);
    allocationPages_.erase(address);
    return true;
}
bool Memory::valid(uint32_t address, uint32_t bytes) const {
    const bool timing = memoryQueryTimingEnabled();
    const uint64_t began = timing ? memoryQueryClock() : 0;
    if (timing) ++memoryQueries.validations;
    const auto complete = [&](bool valid) {
        if (timing) memoryQueries.validationNanoseconds += memoryQueryClock() - began;
        return valid;
    };
    if (address < 0x10000 || !bytes || uint64_t(address) + bytes > kAddressSpace) return complete(false);
    const bool resident = residentReadWritePages(base_, address, bytes, validationPageSize(),
        [&](PSAPI_WORKING_SET_EX_INFORMATION* entries, unsigned pages) {
            const uint64_t queryBegan = timing ? memoryQueryClock() : 0;
            const BOOL queried = QueryWorkingSetEx(GetCurrentProcess(), entries, DWORD(pages * sizeof(entries[0])));
            if (timing) {
                ++memoryQueries.queries;
                const uint64_t elapsed = memoryQueryClock() - queryBegan;
                memoryQueries.queryNanoseconds += elapsed;
                recordMemoryQueryApi(true, elapsed);
            }
            return queried != FALSE;
        });
    if (resident) return complete(true);
    uint64_t cursor = address, end = uint64_t(address) + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        const uint64_t queryBegan = timing ? memoryQueryClock() : 0;
        const SIZE_T queried = VirtualQuery(base_ + cursor, &info, sizeof(info));
        if (timing) {
            ++memoryQueries.queries;
            const uint64_t queryNanoseconds = memoryQueryClock() - queryBegan;
            memoryQueries.queryNanoseconds += queryNanoseconds;
            recordMemoryQueryApi(false, queryNanoseconds);
            const MemoryQueryRegionObservation observation{
                {uint64_t(reinterpret_cast<uintptr_t>(info.BaseAddress)), uint64_t(info.RegionSize),
                    info.State, info.Protect, queried != 0}, address, bytes, uint32_t(cursor)};
            if (memoryQueryRegions.record(observation, queryNanoseconds)) reportMemoryQueryRegions();
        }
        if (!queried || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return complete(false);
        cursor = uint64_t(static_cast<uint8_t*>(info.BaseAddress) - base_) + info.RegionSize;
    }
    return complete(true);
}
bool Memory::validPair(uint32_t firstAddress, uint32_t firstBytes, uint32_t secondAddress, uint32_t secondBytes) const {
    const bool timing = memoryQueryTimingEnabled();
    const uint64_t began = timing ? memoryQueryClock() : 0;
    if (timing) ++memoryQueries.validations;
    const bool resident = residentReadWritePagePair(base_, firstAddress, firstBytes, secondAddress, secondBytes,
        validationPageSize(), [&](PSAPI_WORKING_SET_EX_INFORMATION* entries, unsigned pages) {
            const uint64_t queryBegan = timing ? memoryQueryClock() : 0;
            const BOOL queried = QueryWorkingSetEx(GetCurrentProcess(), entries, DWORD(pages * sizeof(entries[0])));
            if (timing) {
                ++memoryQueries.queries;
                const uint64_t elapsed = memoryQueryClock() - queryBegan;
                memoryQueries.queryNanoseconds += elapsed;
                recordMemoryQueryApi(true, elapsed);
            }
            return queried != FALSE;
        });
    // Finish the joint probe's accounting before any scalar fallback, whose
    // existing counters then cover its own nonoverlapping validation work.
    if (timing) memoryQueries.validationNanoseconds += memoryQueryClock() - began;
    return resident || (valid(firstAddress, firstBytes) && valid(secondAddress, secondBytes));
}
uint32_t Memory::read32(uint32_t address) const {
    if (!valid(address, 4)) fail("Native import read of inaccessible guest memory");
    uint32_t value; memcpy(&value, base_ + address, 4); return _byteswap_ulong(value);
}
void Memory::write32(uint32_t address, uint32_t value) {
    if (!valid(address, 4)) fail("Native import write of inaccessible guest memory");
    value = _byteswap_ulong(value); memcpy(base_ + address, &value, 4);
}
uint64_t Memory::read64(uint32_t address) const {
    if (!valid(address, 8)) fail("Native import read of inaccessible guest memory");
    uint64_t value; memcpy(&value, base_ + address, 8); return _byteswap_uint64(value);
}
void Memory::write64(uint32_t address, uint64_t value) {
    if (!valid(address, 8)) fail("Native import write of inaccessible guest memory");
    value = _byteswap_uint64(value); memcpy(base_ + address, &value, 8);
}
void Memory::initializeDataImports(const LoadedXex& image) {
    uint32_t timestampAddress = 0;
    // Kind-0 records paired with a callable thunk are resolver slots, not data exports.
    for (const auto& record : image.imports) {
        if (record.kind != 0) continue;
        bool callable = false;
        for (const auto& other : image.imports) if (other.kind == 1 && record.library == other.library && record.ordinal == other.ordinal) callable = true;
        if (callable) continue;
        uint32_t address = 0;
        if (record.name == "KeTimeBaseFrequency") { address = allocate(256); write64(address, 49875000); }
        else if (record.name == "XexExecutableModuleHandle") {
            // The loader entry is a guest structure; executable headers are available at +88.
            uint32_t module = allocate(256);
            address = allocate(256);
            write32(module + 88, 0x81000000);
            write32(address, module);
        } else if (record.name == "ExLoadedCommandLine") {
            address = allocate(256);
            const char commandLine[] = "default.xex";
            memcpy(base_ + address, commandLine, sizeof(commandLine));
        } else if (record.name == "XboxHardwareInfo") {
            // Retail Xbox ABI profile used by this title's debug-allocation branch.
            address = allocate(256); write32(address, 0x20); base_[address + 4] = 6;
        } else if (record.name == "XboxKrnlVersion") {
            // The compatibility version encoded in this original XEX's import libraries.
            address = allocate(256); write32(address, 2u << 16); write32(address + 4, 11626u << 16);
        } else if (record.name == "KeDebugMonitorData" || record.name == "KeCertMonitorData") {
            address = allocate(256); // Exported optional observer pointer: no observer installed.
        } else if (record.name == "ExEventObjectType" || record.name == "ExThreadObjectType") {
            address = allocate(256);
            write32(address + 24, record.name == "ExEventObjectType" ? 0x45766e74 : 0x54687264);
            configureObjectType(record.name == "ExEventObjectType" ? ObjectKind::Event : ObjectKind::Thread, address);
        } else if (record.name == "KeTimeStampBundle") {
            address = allocate(256); timestampAddress = address;
            write32(address + 16, GetTickCount());
        } else {
            // Resolve to reserved, inaccessible memory. Actual use produces a named
            // failure, while never-read exports need no invented data contents.
            address = 0x08000000u + uint32_t(unsupportedDataImports_.size()) * 65536u;
            unsupportedDataImports_[address] = record.library + "!" + record.name;
            fprintf(stderr, "[DATA BOUNDARY] %s at guest 0x%08X\n", unsupportedDataImports_[address].c_str(), address);
        }
        write32(record.address, address);
    }
    if (timestampAddress) timestampThread_ = std::jthread([this, timestampAddress](std::stop_token stop) {
        while (!stop.stop_requested()) {
            InterlockedExchange(reinterpret_cast<volatile LONG*>(base_ + timestampAddress + 16),
                LONG(_byteswap_ulong(GetTickCount())));
            Sleep(2);
        }
    });
}
std::string Memory::unsupportedDataImport(uint32_t address) const {
    auto found = unsupportedDataImports_.upper_bound(address);
    if (found == unsupportedDataImports_.begin()) return {};
    --found;
    return uint64_t(address) - found->first < 65536u ? found->second : std::string();
}
void Memory::initializeThread(PPCContext& ctx, uint32_t stackSize) {
    ctx = {};
    stackSize = std::max(stackSize, 1024u * 1024u);
    uint32_t stack = allocate(stackSize);
    uint32_t pcr = allocate(0x3000);
    uint32_t tls = allocate(64 * 4);
    ctx.r1.u32 = (stack + stackSize - 256) & ~15u;
    ctx.r13.u32 = pcr;
    ctx.fpscr.loadFromHost();
    write32(pcr, tls);
    write64(pcr + 0x30, pcr);
    write32(pcr + 0x70, stack + stackSize);
    write32(pcr + 0x74, stack);
    write32(pcr + 0x100, pcr + 0xd00);
    write32(pcr + 0x2a8, pcr + 0x100);
    write32(pcr + 0xd00 + 0x5c, stack + stackSize);
    write32(pcr + 0xd00 + 0x60, stack);
    write32(pcr + 0xd00 + 0x68, tls);
    write32(pcr + 0xd00 + 0x14c, GetCurrentThreadId());
}
LONG exceptionFilter(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode == 0xe06d7363) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "[HOST FAULT] code=0x%08lX address=%p\n", exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress);
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        fprintf(stderr, "guest memory fault operation=%llu host_address=0x%llX\n", exception->ExceptionRecord->ExceptionInformation[0], exception->ExceptionRecord->ExceptionInformation[1]);
        if (memory) {
            uint64_t hostAddress = exception->ExceptionRecord->ExceptionInformation[1];
            uint64_t guestAddress = hostAddress - uint64_t(memory->base());
            if (guestAddress < kAddressSpace) {
                auto name = memory->unsupportedDataImport(uint32_t(guestAddress));
                if (!name.empty()) fprintf(stderr, "[MISSING NATIVE DATA IMPORT] %s\n", name.c_str());
            }
        }
    }
    if (currentContext) printContext(*currentContext);
    return EXCEPTION_EXECUTE_HANDLER;
}
}

[[noreturn]] void PPCRecompFailure(const PPCContext& ctx, uint32_t address, const char* reason) {
    fprintf(stderr, "[TRANSLATION] guest=0x%08X reason=%s\n", address, reason);
    Doritos::Native::printContext(ctx);
    Doritos::Native::fail(reason);
}
[[noreturn]] void PPCMemoryFailure(uint32_t address, const char* reason) {
    fprintf(stderr, "[MEMORY] guest=0x%08X\n", address);
    Doritos::Native::fail(reason);
}
uint64_t PPCQueryTimebase() {
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
    return uint64_t(counter.QuadPart / frequency.QuadPart) * 49875000ull +
        uint64_t(counter.QuadPart % frequency.QuadPart) * 49875000ull / frequency.QuadPart;
}
uint64_t PPCReadMmio(uint32_t address, unsigned width) {
    uint64_t value = 0;
    if (Doritos::Native::mmioReadHandler && Doritos::Native::mmioReadHandler(address, width, value)) return value;
    fprintf(stderr, "[MMIO READ] guest=0x%08X width=%u\n", address, width);
    Doritos::Native::fail("Native GPU/MMIO boundary has not been implemented");
}
void PPCWriteMmio(uint32_t address, uint64_t value, unsigned width) {
    if (Doritos::Native::mmioWriteHandler && Doritos::Native::mmioWriteHandler(address, value, width)) return;
    fprintf(stderr, "[MMIO WRITE] guest=0x%08X value=0x%llX width=%u\n", address, value, width);
    Doritos::Native::fail("Native GPU/MMIO boundary has not been implemented");
}
