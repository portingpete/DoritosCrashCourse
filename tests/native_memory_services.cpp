#include "runtime/native/runtime.h"
#include "runtime/native/memory_services.h"
#include "runtime/native/reservation_transaction.h"
#include "runtime/native/reservation_gap.h"
#include "runtime/native/resident_page_validation.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <new>
#include <stdexcept>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
using namespace Doritos::Native;
namespace {
void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
void residentPageContracts() {
    constexpr uint32_t page = 4096, address = 0x10000000;
    constexpr DWORD sentinel = 0x53ab719e;
    // Synthetic provider fills only OS-result records; target addresses are
    // integer identifiers and are never read, written or passed to Windows.
    const auto* base = reinterpret_cast<const uint8_t*>(uintptr_t(0x100000000ull));
    unsigned calls = 0;
    auto allResident = [&](PSAPI_WORKING_SET_EX_INFORMATION* entries, unsigned count) {
        ++calls;
        for (unsigned index = 0; index < count; ++index) {
            entries[index].VirtualAttributes.Flags = 0;
            entries[index].VirtualAttributes.Valid = 1;
            entries[index].VirtualAttributes.Win32Protection = PAGE_READWRITE;
        }
        SetLastError(ERROR_ACCESS_DENIED);
        return true;
    };
    SetLastError(sentinel);
    require(residentReadWritePages(base, address + page - 2, 4, page,
        [&](auto* entries, unsigned count) {
            require(count == 2 && uintptr_t(entries[0].VirtualAddress) == uintptr_t(base) + address &&
                uintptr_t(entries[1].VirtualAddress) == uintptr_t(base) + address + page,
                "unaligned span queries exactly its two intersecting pages");
            return allResident(entries, count);
        }) && GetLastError() == sentinel, "all-page exact resident RW proof preserves incoming last error");
    require(residentReadWritePages(base, address + page - 4, 4, page, [&](auto* entries, unsigned count) {
        require(count == 1, "exact byte endpoint excludes the following page"); return allResident(entries, count);
    }), "exact page endpoint positive proof");
    for (unsigned pages : {16u,17u,87u,residentPageValidationLimit})
        require(residentReadWritePages(base, address, page * pages, page, [&](auto* entries, unsigned count) {
            require(count == pages, "bounded batch queries every intersecting page");
            for (unsigned index = 0; index < count; ++index)
                require(uintptr_t(entries[index].VirtualAddress) == uintptr_t(base) + address + index * page,
                    "every batched page address follows its original ascending offset");
            return allResident(entries, count);
        }), "aligned small and large batches provide a fresh positive proof");
    require(residentReadWritePages(base,address + 1,page * 16,page,[&](auto* entries,unsigned count) {
        require(count == 17,"unaligned old capacity includes its seventeenth page");
        return allResident(entries,count);
    }),"unaligned small-to-large batch transition");
    for (unsigned badPage : {0u,15u,16u,64u,residentPageValidationLimit - 1})
    for (unsigned mode = 0; mode < 7; ++mode) {
        SetLastError(sentinel);
        require(!residentReadWritePages(base, address, page * residentPageValidationLimit, page, [&](auto* entries, unsigned count) {
            require(count == residentPageValidationLimit, "bounded maximum includes every intersecting page");
            allResident(entries, count);
            auto& last = entries[badPage].VirtualAttributes;
            if (mode == 0) last.Valid = 0;
            if (mode == 1) last.Bad = 1;
            if (mode == 2) last.LargePage = 1;
            if (mode == 3) last.Shared = 1;
            if (mode == 4) last.Win32Protection = PAGE_READWRITE | PAGE_GUARD;
            if (mode == 5) last.Win32Protection = PAGE_READONLY;
            if (mode == 6) last.Win32Protection = PAGE_READWRITE | PAGE_NOCACHE;
            return true;
        }) && GetLastError() == sentinel, "first/middle/final uncertain/protected/special page defers to original VQ");
    }
    SetLastError(sentinel);
    require(!residentReadWritePages(base, address, 4, page, [&](auto* entries, unsigned count) {
        allResident(entries, count); return false;
    }) && GetLastError() == sentinel, "API failure ignores stale positive records and restores last error before fallback");
    const unsigned before = calls;
    require(!residentReadWritePages(base, address + 1, page * residentPageValidationLimit, page, allResident) &&
        !residentReadWritePages(base, address, page * (residentPageValidationLimit + 1), page, allResident) &&
        !residentReadWritePages(base, address, 0, page, allResident) &&
        !residentReadWritePages(base, 0xffff, 4, page, allResident) &&
        !residentReadWritePages(base, 0xffffffff, 2, page, allResident) &&
        !residentReadWritePages(base, address, 4, 0, allResident) &&
        !residentReadWritePages(base, address, 4, 3000, allResident) &&
        !residentReadWritePages(reinterpret_cast<const uint8_t*>(uintptr_t(base) + 1), address, 4, page, allResident) &&
        !residentReadWritePages(reinterpret_cast<const uint8_t*>(UINTPTR_MAX & ~uintptr_t(page - 1)), address, 4, page, allResident) &&
        calls == before, "bound, byte, guest, granularity, alignment and host-overflow rejection performs no working-set query");
    require(residentReadWritePages(base, 0xffffffff, 1, page, [&](auto* entries, unsigned count) {
        require(count == 1 && uintptr_t(entries[0].VirtualAddress) == uintptr_t(base) + 0xfffff000ull,
            "four-GiB exclusive endpoint forms only its final actual page");
        return allResident(entries, count);
    }), "four-GiB endpoint is supported without wrapping");
    bool valid = true;
    auto changes = [&](auto* entries, unsigned count) {
        allResident(entries, count); entries[0].VirtualAttributes.Valid = valid; return true;
    };
    require(residentReadWritePages(base, address, 4, page, changes), "first current resident snapshot accepted");
    valid = false;
    require(!residentReadWritePages(base, address, 4, page, changes), "next invocation observes changed status without a validity cache");
    valid = true;
    require(residentReadWritePages(base, address, 4, page, changes), "a later current snapshot can be accepted fresh");
    SetLastError(sentinel);
    require(residentReadWritePagePair(base,address,4,address + 100,4,page,[&](auto* entries,unsigned count) {
        require(count == 1 && uintptr_t(entries[0].VirtualAddress) == uintptr_t(base) + address,
            "two spans on one page query that page once");
        return allResident(entries,count);
    }) && GetLastError() == sentinel,"paired positive proof preserves incoming last error");
    require(residentReadWritePagePair(base,address + page - 2,4,address + page,2 * page,page,
        [&](auto* entries,unsigned count) {
            require(count == 3,"overlapping unaligned spans query their union only");
            for (unsigned index = 0; index < count; ++index)
                require(uintptr_t(entries[index].VirtualAddress) == uintptr_t(base) + address + index * page,
                    "paired union contains every exact intersecting page");
            return allResident(entries,count);
        }),"paired proof covers overlap and both partial endpoints");
    require(residentReadWritePagePair(base,address,4,address + 20 * page,4,page,
        [&](auto* entries,unsigned count) {
            require(count == 2 && uintptr_t(entries[1].VirtualAddress) == uintptr_t(base) + address + 20 * page,
                "disjoint spans do not query the unused gap");
            return allResident(entries,count);
        }),"disjoint pair has no contiguity requirement");
    require(residentReadWritePagePair(base,address,16 * page,address + page,15 * page,page,
        [&](auto* entries,unsigned count) {
            require(count == 16,"overlapping capacity counts unique pages");
            return allResident(entries,count);
        }),"paired capacity includes shared pages once");
    const unsigned pairBefore = calls;
    require(!residentReadWritePagePair(base,address,8 * page,address + 20 * page,9 * page,page,allResident) &&
        !residentReadWritePagePair(base,address,17 * page,address,4,page,allResident) &&
        !residentReadWritePagePair(base,0xffff,4,address,4,page,allResident) &&
        !residentReadWritePagePair(base,address,4,0xffffffff,2,page,allResident) &&
        !residentReadWritePagePair(base,address,0,address,4,page,allResident) &&
        !residentReadWritePagePair(base,address,4,address,0,page,allResident) &&
        !residentReadWritePagePair(base,address,4,address,4,3000,allResident) &&
        !residentReadWritePagePair(nullptr,address,4,address,4,page,allResident) &&
        !residentReadWritePagePair(reinterpret_cast<const uint8_t*>(uintptr_t(base) + 1),address,4,address,4,page,allResident) &&
        !residentReadWritePagePair(reinterpret_cast<const uint8_t*>(UINTPTR_MAX & ~uintptr_t(page - 1)),address,4,address,4,page,allResident) &&
        calls == pairBefore,"unprovable pair geometry falls back without querying");
    require(residentReadWritePagePair(base,address,4,0xffffffff,1,page,[&](auto* entries,unsigned count) {
        require(count == 2 && uintptr_t(entries[1].VirtualAddress) == uintptr_t(base) + 0xfffff000ull,
            "paired four-GiB endpoint does not wrap");
        return allResident(entries,count);
    }),"paired final address-space byte is supported");
    for (unsigned badPage = 0; badPage < 3; ++badPage)
    for (unsigned mode = 0; mode < 7; ++mode) {
        SetLastError(sentinel);
        require(!residentReadWritePagePair(base,address,page,address + 20 * page,2 * page,page,
            [&](auto* entries,unsigned count) {
                require(count == 3,"both disjoint pair spans are queried");
                allResident(entries,count);
                auto& attributes = entries[badPage].VirtualAttributes;
                if (mode == 0) attributes.Valid = 0;
                if (mode == 1) attributes.Bad = 1;
                if (mode == 2) attributes.LargePage = 1;
                if (mode == 3) attributes.Shared = 1;
                if (mode == 4) attributes.Win32Protection = PAGE_READWRITE | PAGE_GUARD;
                if (mode == 5) attributes.Win32Protection = PAGE_READONLY;
                if (mode == 6) attributes.Win32Protection = PAGE_READWRITE | PAGE_NOCACHE;
                return true;
            }) && GetLastError() == sentinel,"uncertain first/second span page cannot prove a pair");
    }
    require(!residentReadWritePagePair(base,address,4,address + page,4,page,[&](auto* entries,unsigned count) {
        allResident(entries,count); return false;
    }) && GetLastError() == sentinel,"paired failed API defers and preserves last error");
    valid = true;
    require(residentReadWritePagePair(base,address,4,address + page,4,page,changes),"first pair is freshly proven");
    valid = false;
    require(!residentReadWritePagePair(base,address,4,address + page,4,page,changes),"pair does not retain a previous positive proof");
}
void residentPairTransitions(Memory& guest) {
    constexpr uint32_t page = 4096;
    const uint32_t first = guest.allocate(2 * page), second = guest.allocate(20 * page);
    require(guest.validPair(first,4,second,4),"untouched paired allocations retain scalar acceptance");
    guest.write32(first,0x12345678); guest.write32(second,0x87654321);
    require(guest.validPair(first,4,second,4) && guest.validPair(first,4,first + 8,4),
        "resident disjoint and same-page production pairs are accepted");
    require(guest.validPair(first,4,second,20 * page),"oversized pair retains ordinary scalar fallback");
    DWORD previous = 0;
    MEMORY_BASIC_INFORMATION info{};
    for (uint32_t changed : {first,second}) {
        require(VirtualProtect(guest.base() + changed,page,PAGE_READWRITE | PAGE_GUARD,&previous),"pair guard setup");
        require(!guest.validPair(first,4,second,4),"fresh pair rejects guarded first or second span");
        require(VirtualQuery(guest.base() + changed,&info,sizeof(info)) && (info.Protect & PAGE_GUARD),
            "joint proof and scalar fallback leave one-shot guard unconsumed");
        require(VirtualProtect(guest.base() + changed,page,PAGE_READONLY,&previous) && guest.validPair(first,4,second,4),
            "pair retains scalar read-only acceptance");
        require(VirtualProtect(guest.base() + changed,page,PAGE_NOACCESS,&previous) && !guest.validPair(first,4,second,4),
            "pair rejects newly inaccessible first or second span");
        require(VirtualProtect(guest.base() + changed,page,PAGE_READWRITE,&previous) && guest.validPair(first,4,second,4),
            "pair observes restored protection");
        require(guest.decommit(changed,page) && !guest.validPair(first,4,second,4),"pair observes first or second decommit");
        guest.commit(changed,page);
        require(guest.validPair(first,4,second,4) && guest.read32(changed) == 0,"pair observes untouched recommit");
        guest.write32(changed,0x12345678);
    }
    require(!guest.validPair(first,4,0xffff,4) && !guest.validPair(first,0,second,4) &&
        !guest.validPair(first,4,0xffffffff,2),"invalid paired range retains scalar rejection");
    require(guest.release(second) && !guest.validPair(first,4,second,4),"pair cannot outlive a released second allocation");
    require(guest.release(first) && !guest.validPair(first,4,first,4),"pair cannot outlive a released first allocation");
}
void residentMemoryTransitions(Memory& guest) {
    const uint32_t span = guest.allocate(8192);
    require(guest.valid(span, 8192), "untouched ordinary commits retain original valid acceptance");
    guest.write32(span, 0x12345678); guest.write32(span + 4096, 0x87654321);
    require(guest.valid(span + 4094, 4), "resident unaligned production span remains valid");
    DWORD previous = 0;
    require(VirtualProtect(guest.base() + span + 4096, 4096, PAGE_READWRITE | PAGE_GUARD, &previous), "guard transition setup");
    require(!guest.valid(span + 4094, 4), "fresh resident path cannot accept a newly guarded final page");
    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(guest.base() + span + 4096, &info, sizeof(info)) && (info.Protect & PAGE_GUARD),
        "production validity query leaves one-shot guard protection unconsumed");
    require(VirtualProtect(guest.base() + span + 4096, 4096, PAGE_READONLY, &previous) && guest.valid(span + 4096, 4),
        "read-only pages retain original VQ acceptance through conservative fallback");
    require(VirtualProtect(guest.base() + span + 4096, 4096, PAGE_READWRITE, &previous) &&
        guest.decommit(span + 4096, 4096) && !guest.valid(span + 4096, 4), "fresh production checks observe decommit");
    guest.commit(span + 4096, 4096);
    require(guest.valid(span + 4096, 4) && guest.read32(span + 4096) == 0, "untouched recommit remains valid with new zero backing");
    require(guest.release(span) && !guest.valid(span, 1), "released owned extent remains inaccessible");

    constexpr uint32_t page = 4096, largeBytes = residentPageValidationLimit * page;
    const uint32_t large = guest.allocate(largeBytes + page);
    require(guest.valid(large,largeBytes),"untouched large span remains valid through conservative fallback");
    for (uint32_t offset = 0; offset < largeBytes + page; offset += page)
        guest.write32(large + offset,0x12345678);
    require(guest.valid(large,largeBytes) && guest.valid(large + 1,largeBytes - 1),
        "large resident aligned and unaligned spans check all pages freshly");
    require(guest.valid(large + 1,largeBytes) && guest.valid(large,largeBytes + page),
        "oversized resident spans retain original fallback acceptance");
    for (unsigned pageIndex : {0u,15u,16u,64u,residentPageValidationLimit - 1}) {
        const uint32_t changed = large + pageIndex * page;
        require(VirtualProtect(guest.base() + changed,page,PAGE_READWRITE | PAGE_GUARD,&previous),
            "large-span guard setup");
        require(!guest.valid(large,largeBytes),"large proof observes a newly guarded first/middle/final page");
        require(VirtualQuery(guest.base() + changed,&info,sizeof(info)) && (info.Protect & PAGE_GUARD),
            "large batch and fallback leave one-shot guard unconsumed");
        require(VirtualProtect(guest.base() + changed,page,PAGE_READONLY,&previous) && guest.valid(large,largeBytes),
            "large readonly subregion remains accepted by original fallback");
        require(VirtualProtect(guest.base() + changed,page,PAGE_NOACCESS,&previous) && !guest.valid(large,largeBytes),
            "large batch cannot accept an inaccessible interior subregion");
        require(VirtualProtect(guest.base() + changed,page,PAGE_READWRITE,&previous) && guest.valid(large,largeBytes),
            "large span observes restored protection without a cached failure");
    }
    require(guest.decommit(large + 64 * page,page) && !guest.valid(large,largeBytes),
        "large span observes an interior decommit after successful proofs");
    guest.commit(large + 64 * page,page);
    require(guest.valid(large,largeBytes) && guest.read32(large + 64 * page) == 0,
        "large span observes untouched zeroed recommit through original fallback");
    require(guest.release(large) && !guest.valid(large,largeBytes),
        "large span validity does not survive allocation release");
}
struct AllocationFault {
    size_t calls = 0, live = 0, failAt = 0;
};
template<class T> struct FaultAllocator {
    using value_type = T;
    AllocationFault* fault;
    explicit FaultAllocator(AllocationFault* value) noexcept : fault(value) {}
    template<class U> FaultAllocator(const FaultAllocator<U>& other) noexcept : fault(other.fault) {}
    T* allocate(size_t count) {
        if (++fault->calls == fault->failAt) throw std::bad_alloc();
        T* result = std::allocator<T>{}.allocate(count);
        fault->live += count;
        return result;
    }
    void deallocate(T* pointer, size_t count) noexcept {
        fault->live -= count;
        std::allocator<T>{}.deallocate(pointer, count);
    }
    template<class U> bool operator==(const FaultAllocator<U>& other) const noexcept { return fault == other.fault; }
};
using FaultMap = std::map<uint32_t, uint32_t, std::less<uint32_t>, FaultAllocator<std::pair<const uint32_t, uint32_t>>>;
void reservationGapContracts() {
    const std::map<uint32_t,uint32_t> allocations{{0x10000000,0x2000},{0x10010000,0x10000},{0x5fff0000,0x10000}};
    const std::map<uint32_t,uint32_t> fixed{{0x10004000,0x8000},{0x81000000,0x10000}};
    require(findGuestReservationGap(allocations,fixed,4096,4096,0x10000000,0x60000000) == 0x10002000,
        "ordinary gaps include released pages while excluding live and fixed mappings");
    require(findGuestReservationGap(allocations,fixed,65536,65536,0x10001000,0x60000000) == 0x10020000,
        "an aligned request skips a containing live owner and intermediate fixed mapping");
    require(findGuestReservationGap(allocations,fixed,4096,4096,0x10003000,0x1000c000) == 0x10003000 &&
        findGuestReservationGap(allocations,fixed,8192,4096,0x10003000,0x1000c000) == 0,
        "adjacent fixed mapping bounds the actual available gap without overlap");
    const std::map<uint32_t,uint32_t> crossing{{0x10000000,0x3000}};
    const std::map<uint32_t,uint32_t> overlay{{0x10001000,0x5000}};
    require(findGuestReservationGap(crossing,overlay,4096,4096,0x10002000,0x10007000) == 0x10006000,
        "merged live and fixed ownership intervals exclude their full overlapping union");
    const std::map<uint32_t,uint32_t> empty;
    require(findGuestReservationGap(empty,empty,4096,4096,0xffffe001,0x100000000ull) == 0xfffff000 &&
        findGuestReservationGap(empty,empty,8192,4096,0xffffe001,0x100000000ull) == 0 &&
        findGuestReservationGap(empty,empty,4096,65536,0xfffff000,0x100000000ull) == 0 &&
        findGuestReservationGap(empty,empty,0x100000000ull,4096,0x10000000,0x60000000) == 0,
        "alignment and byte sums cannot wrap or escape the containing 32-bit guest interval");
}
void reservationTransactionContracts() {
    // Real std::map node allocations fail at either insertion in the exact
    // production helper, without a Memory fault hook or global heap override.
    for (const bool advancesCursor : {false, true}) {
        AllocationFault fault;
        FaultMap allocations(std::less<uint32_t>{}, FaultAllocator<std::pair<const uint32_t, uint32_t>>(&fault));
        FaultMap pages(std::less<uint32_t>{}, FaultAllocator<std::pair<const uint32_t, uint32_t>>(&fault));
        allocations.emplace(0x10000000, 4096); pages.emplace(0x10000000, 4096);
        allocations.emplace(0x10004000, 8192); // Existing allocation only.
        pages.emplace(0x10008000, 65536); // Existing page record only.
        const std::map<uint32_t, uint32_t> initialAllocations(allocations.begin(), allocations.end());
        const std::map<uint32_t, uint32_t> initialPages(pages.begin(), pages.end());
        const size_t initialLive = fault.live;
        uint32_t cursor = 0x20000000;
        auto unchanged = [&] {
            return cursor == 0x20000000 && fault.live == initialLive &&
                allocations.size() == initialAllocations.size() && pages.size() == initialPages.size() &&
                std::equal(allocations.begin(), allocations.end(), initialAllocations.begin()) &&
                std::equal(pages.begin(), pages.end(), initialPages.begin());
        };
        for (const size_t failAt : {size_t(1), size_t(2)}) {
            fault.calls = 0; fault.failAt = failAt;
            bool rejected = false;
            try { recordGuestReservation(allocations, pages, 0x20000000, 65536, 65536, advancesCursor ? &cursor : nullptr); }
            catch (const std::bad_alloc&) { rejected = true; }
            require(rejected && fault.calls == failAt && unchanged(),
                "either reservation node allocation failure preserves both maps, live nodes and cursor");
        }
        fault.calls = 0; fault.failAt = 0;
        for (const uint32_t duplicate : {0x10000000u, 0x10004000u, 0x10008000u}) {
            require(!recordGuestReservation(allocations, pages, duplicate, 12288, 8192, advancesCursor ? &cursor : nullptr) && unchanged(),
                "duplicate in either or both maps preserves existing values and cursor without an orphan node");
        }
        require(recordGuestReservation(allocations, pages, 0x20000000, 65536, 65536, advancesCursor ? &cursor : nullptr),
            "reservation retries successfully after map allocation failures");
        require(allocations.at(0x20000000) == 65536 && pages.at(0x20000000) == 65536 &&
            allocations.size() == initialAllocations.size() + 1 && pages.size() == initialPages.size() + 1 &&
            cursor == (advancesCursor ? 0x20010000u : 0x20000000u),
            "successful reservation publishes both records and advances only the supplied cursor");
    }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        reservationTransactionContracts();
        reservationGapContracts();
        residentPageContracts();
        const auto image = loadOriginalXex(argv[1]);
        Memory guest(image); memory = &guest;
        residentMemoryTransitions(guest);
        residentPairTransitions(guest);
        initializeMemoryServices();
        PPCContext ctx{}; guest.initializeThread(ctx, 0x100000); currentContext = &ctx;
        const uint32_t output = guest.allocate(4096);
        const uint32_t cursorReservation = guest.reserve(4096);
        const uint32_t boundedReservation = guest.reserveRange(8192, 4096, 4096, 0x60000000, 0x60002000);
        require(boundedReservation == 0x60000000 && guest.allocationSize(boundedReservation) == 8192 &&
            guest.reservationPageSize(boundedReservation + 4096) == 4096 && !guest.valid(boundedReservation, 1),
            "bounded reservation publishes matching ownership/page records without committing backing");
        const auto beforeReservationFailure = guest.reservationSnapshot();
        require(guest.reserveRange(4096, 4096, 4096, 0x60000000, 0x60002000) == 0 &&
            guest.reservationSnapshot() == beforeReservationFailure && guest.reservationPageSize(boundedReservation) == 4096,
            "exhausted bounded reservation leaves both existing records unchanged");
        bool exhausted = false;
        try { guest.reserve(UINT32_MAX); } catch (const NativeFailure&) { exhausted = true; }
        require(exhausted && guest.reservationSnapshot() == beforeReservationFailure &&
            guest.reserve(4096) == cursorReservation + 4096,
            "failed ordinary reservation and bounded allocations leave the ordinary cursor unchanged");
        require(guest.release(cursorReservation) && guest.release(cursorReservation + 4096) && guest.release(boundedReservation) &&
            guest.reservationPageSize(cursorReservation) == 0 && guest.reservationPageSize(boundedReservation) == 0,
            "reservation release removes both transaction records");
        // Reserve a genuine uncommitted 1 GiB extent, with live sentinels on
        // both sides. Its release must make it allocatable again even though
        // no second 1 GiB extent fits above the ordinary allocation cursor.
        const uint32_t reuseBefore = guest.allocate(4096);
        guest.write32(reuseBefore,0x12345678);
        const uint32_t largeExtent = guest.reserve(0x40000000,65536,65536);
        const uint32_t reuseAfter = guest.allocate(4096);
        guest.write32(reuseAfter,0xabcdef01);
        guest.commit(largeExtent,65536); guest.write32(largeExtent,0xfeedface);
        require(guest.decommit(largeExtent,65536) && !guest.valid(largeExtent,1),
            "partial decommit discards backing while retaining its complete large reservation");
        const auto beforeOwnedDecommitFailure = guest.reservationSnapshot();
        bool retainedExtentExcluded = false;
        try { guest.reserve(0x40000000,65536,65536); } catch (const NativeFailure&) { retainedExtentExcluded = true; }
        require(retainedExtentExcluded && guest.reservationSnapshot() == beforeOwnedDecommitFailure &&
            guest.reservationContains(largeExtent,0x40000000) && guest.reservationPageSize(largeExtent) == 65536,
            "ordinary wrap cannot reuse decommitted pages still owned by a live reservation or mutate ownership on exhaustion");
        guest.commit(largeExtent,65536); guest.write32(largeExtent,0xfeedface);
        require(guest.release(largeExtent) && !guest.valid(largeExtent,1),
            "released large extent loses backing and reservation ownership");
        const uint32_t reusedExtent = guest.reserve(0x40000000,65536,65536);
        require(reusedExtent == largeExtent && guest.allocationSize(reusedExtent) == 0x40000000 &&
            guest.reservationPageSize(reusedExtent) == 65536 && !guest.valid(reusedExtent,1),
            "ordinary reservation wraps to released extent with exact alignment and page ownership");
        guest.commit(reusedExtent,65536);
        require(guest.read32(reusedExtent) == 0 && guest.read32(reuseBefore) == 0x12345678 && guest.read32(reuseAfter) == 0xabcdef01,
            "reused pages begin zeroed and do not change neighboring live owners");
        require(guest.release(reusedExtent) && guest.release(reuseBefore) && guest.release(reuseAfter),
            "large-extent reuse contract releases only its own test reservations");
        const uint32_t aheadCursor = guest.reserve(4096);
        const uint32_t aheadOwner = guest.reserveRange(8192,4096,4096,aheadCursor + 4096,uint64_t(aheadCursor) + 12288);
        require(aheadOwner == aheadCursor + 4096,"bounded allocation can own the next ordinary cursor address");
        const uint32_t skippedOwner = guest.reserve(4096);
        require(skippedOwner == aheadCursor + 12288 && guest.allocationSize(aheadOwner) == 8192 &&
            guest.reservationContains(aheadOwner,8192),
            "ordinary allocation skips an independently reserved live extent ahead of its cursor");
        require(guest.release(aheadCursor) && guest.release(aheadOwner) && guest.release(skippedOwner),
            "bounded-cursor exclusion contract releases its exact test ownership ranges");
        auto call = [&](const char* name) {
            require(memoryServicesImport(ctx, guest.base(), name), "unrecognized native memory operation");
            return ctx.r3.u32;
        };
        auto allocate = [&](uint32_t size, uint32_t protection = 4, uint32_t alignment = 0, uint32_t flags = 0) {
            ctx.r3.u64 = flags; ctx.r4.u64 = size; ctx.r5.u64 = protection;
            ctx.r6.u64 = 0; ctx.r7.u64 = 0xffffffff; ctx.r8.u64 = alignment;
            return call("MmAllocatePhysicalMemoryEx");
        };
        const uint32_t startup = allocate(0x2000000);
        require(startup >= 0xe0000000 && !(startup & 4095) && guest.valid(startup, 0x2000000), "actual startup 32 MiB CPU buffer contract");
        require(guest.read32(startup) == 0 && guest.read32(startup + 0x2000000 - 4) == 0, "new native committed pages start zeroed");
        guest.write32(startup, 0x12345678); guest.write32(startup + 0x2000000 - 4, 0xabcdef01);
        ctx.r3.u64 = startup; require(call("MmQueryAllocationSize") == 0x2000000, "physical allocation owned rounded size");
        ctx.r3.u64 = startup + 127; const uint32_t physical = call("MmGetPhysicalAddress");
        require(physical == (startup & 0x1fffffff) + 4096 + 127 && ctx.r4.u32 == 0, "E-region physical bias and interior offset");
        require(resolvePhysicalAddress(physical, 64) == startup + 127 && resolvePhysicalAddress(0, 64) == 0,
            "native codec physical resolution retains the actual owned CPU storage");
        ctx.r3.u64 = 2; ctx.r4.u64 = physical; ctx.r5.u64 = 64; ctx.r6.u64 = 0x404;
        require(call("MmMapIoSpace") == startup + 127, "proven XMA map returns actual coherent owned backing");
        const uint32_t largePage = allocate(70000, 0x20000404, 65536);
        require(largePage >= 0xa0000000 && largePage < 0xc0000000 && !(largePage & 65535) && guest.valid(largePage, 131072), "64 KiB physical page region and rounding");
        ctx.r3.u64 = largePage; const uint32_t largePhysical = call("MmGetPhysicalAddress");
        const uint32_t startupPhysical = (startup & 0x1fffffff) + 4096;
        require(uint64_t(startupPhysical) + 0x2000000 <= largePhysical || uint64_t(largePhysical) + 131072 <= startupPhysical,
            "mixed virtual page regions use distinct common physical ranges");
        ctx.r3.u64 = largePage + 32; require(call("MmQueryAddressProtect") == 0x20000404, "physical cache and page flags preserved");
        ctx.r3.u64 = largePage; ctx.r4.u64 = output; ctx.r5.u64 = 0;
        require(call("NtQueryVirtualMemory") == 0 && guest.read32(output) == largePage && guest.read32(output + 4) == largePage &&
            guest.read32(output + 8) == 0x20000404 && guest.read32(output + 12) == 131072 && guest.read32(output + 16) == MEM_COMMIT &&
            guest.read32(output + 20) == 0x20000404 && guest.read32(output + 24) == MEM_PRIVATE, "Xbox virtual query seven BE dwords");
        const uint32_t virtualReservation = guest.reserve(65536, 65536, 65536);
        ctx.r3.u64 = virtualReservation; ctx.r4.u64 = output; ctx.r5.u64 = 3;
        require(call("NtQueryVirtualMemory") == 0 && guest.read32(output + 16) == MEM_RESERVE && guest.read32(output + 20) == 0,
            "actual query third argument process flags and logical reservation state");
        guest.commit(virtualReservation, 4096);
        ctx.r3.u64 = virtualReservation; ctx.r4.u64 = output; ctx.r5.u64 = 0;
        require(call("NtQueryVirtualMemory") == 0 && guest.read32(output + 12) == 4096 && guest.read32(output + 16) == MEM_COMMIT,
            "query reports committed subregion without swallowing reservation tail");
        ctx.r3.u64 = virtualReservation + 4096; ctx.r4.u64 = output; ctx.r5.u64 = 0;
        require(call("NtQueryVirtualMemory") == 0 && guest.read32(output) == virtualReservation + 4096 &&
            guest.read32(output + 4) == virtualReservation && guest.read32(output + 12) == 61440 && guest.read32(output + 16) == MEM_RESERVE,
            "query reports reserved tail and stable allocation owner");
        const uint32_t freeBasePointer = output + 64, freeSizePointer = output + 68;
        auto freeVirtual = [&](uint32_t base, uint32_t size, uint32_t type = MEM_DECOMMIT) {
            guest.write32(freeBasePointer, base); guest.write32(freeSizePointer, size);
            ctx.r3.u64 = freeBasePointer; ctx.r4.u64 = freeSizePointer; ctx.r5.u64 = type; ctx.r6.u64 = 0;
            return call("NtFreeVirtualMemory");
        };
        auto allocateVirtual = [&](uint32_t base, uint32_t size, uint32_t type) {
            guest.write32(freeBasePointer, base); guest.write32(freeSizePointer, size);
            ctx.r3.u64 = freeBasePointer; ctx.r4.u64 = freeSizePointer; ctx.r5.u64 = type;
            ctx.r6.u64 = PAGE_READWRITE; ctx.r7.u64 = 0;
            return call("NtAllocateVirtualMemory");
        };
        require(allocateVirtual(0, 0x300020, 0x60801000) == 0, "original Avatar heap NOZERO null-base commit allocation");
        const uint32_t avatarCpu = guest.read32(freeBasePointer), avatarBytes = guest.read32(freeSizePointer);
        require(avatarCpu && !(avatarCpu & 65535) && avatarBytes == 0x310000 &&
            guest.allocationSize(avatarCpu) == avatarBytes && guest.reservationPageSize(avatarCpu) == 65536 &&
            guest.valid(avatarCpu, avatarBytes), "null commit creates actual owned and committed 64 KiB CPU pages");
        guest.write32(avatarCpu, 0x11223344); guest.write32(avatarCpu + avatarBytes - 4, 0xaabbccdd);
        require(allocateVirtual(avatarCpu + 123, 1, 0x60801000) == 0 && guest.read32(freeBasePointer) == avatarCpu &&
            guest.read32(freeSizePointer) == 65536 && guest.read32(avatarCpu) == 0x11223344 &&
            guest.read32(avatarCpu + avatarBytes - 4) == 0xaabbccdd,
            "NOZERO commit on live backing retains original heap contents and page owner");
        require(freeVirtual(avatarCpu + 65536, 65536) == 0 && !guest.valid(avatarCpu + 65536, 1),
            "NOZERO allocation supports genuine CRT page decommit");
        require(allocateVirtual(avatarCpu + 65536 + 17, 1, 0x60801000) == 0 &&
            guest.valid(avatarCpu + 65536, 65536) && guest.read32(avatarCpu + 65536) == 0 &&
            guest.read32(avatarCpu) == 0x11223344 && guest.read32(avatarCpu + avatarBytes - 4) == 0xaabbccdd,
            "recommit restores genuine OS backing without changing the neighboring live heap");
        require(freeVirtual(avatarCpu, 0, MEM_RELEASE) == 0, "original Avatar failure cleanup can release its real CPU allocation");
        require(allocateVirtual(0, 131072, 0x60002000) == 0, "original reserve-only large-page CRT path remains supported");
        const uint32_t reservedCpu = guest.read32(freeBasePointer);
        require(guest.allocationSize(reservedCpu) == 131072 && !guest.valid(reservedCpu, 1),
            "reserve-only import creates ownership without CPU backing");
        require(allocateVirtual(reservedCpu + 65536, 1, 0x60001000) == 0 && guest.valid(reservedCpu + 65536, 65536) &&
            !guest.valid(reservedCpu, 1), "original fixed commit retains reserved neighboring page");
        require(freeVirtual(reservedCpu, 0, MEM_RELEASE) == 0, "mixed committed/reserved virtual allocation releases completely");
        const auto beforeInvalid = guest.reservationSnapshot();
        require(allocateVirtual(0, UINT32_MAX, 0x60801000) == 0xc0000017 && guest.read32(freeBasePointer) == 0 &&
            guest.read32(freeSizePointer) == UINT32_MAX && guest.reservationSnapshot() == beforeInvalid,
            "overflow allocation fails with unchanged output and no reservation");
        require(allocateVirtual(0, 0, 0x60801000) == 0xc000000d && guest.reservationSnapshot() == beforeInvalid,
            "zero-byte allocation cannot create owned backing");
        guest.write32(freeSizePointer, 65536);
        ctx.r3.u64 = 0x70000000; ctx.r4.u64 = freeSizePointer; ctx.r5.u64 = 0x60801000;
        ctx.r6.u64 = PAGE_READWRITE; ctx.r7.u64 = 0;
        require(call("NtAllocateVirtualMemory") == 0xc0000005 && guest.read32(freeSizePointer) == 65536 &&
            guest.reservationSnapshot() == beforeInvalid, "invalid allocation in/out parameter fails before reserving pages");
        const uint32_t splitHeap = guest.allocate(4 * 4096);
        const uint32_t adjacentHeap = guest.allocate(4096);
        require(adjacentHeap == splitHeap + 4 * 4096, "independent neighboring reservation for bounds contract");
        for (unsigned page = 0; page < 4; ++page) {
            guest.write32(splitHeap + page * 4096, 0x12345000 + page);
            guest.write32(splitHeap + (page + 1) * 4096 - 4, 0xabcdef00 + page);
        }
        guest.write32(adjacentHeap, 0x87654321);
        require(freeVirtual(splitHeap + 8191, 2) == 0 && guest.read32(freeBasePointer) == splitHeap + 4096 &&
            guest.read32(freeSizePointer) == 8192, "decommit rounds both intersected pages and returns BE range");
        require(guest.allocationSize(splitHeap) == 16384 && guest.reservationContains(splitHeap + 4096, 8192) &&
            !guest.valid(splitHeap + 4096, 1) && !guest.valid(splitHeap + 8192, 1) &&
            guest.read32(splitHeap) == 0x12345000 && guest.read32(splitHeap + 16380) == 0xabcdef03,
            "decommit retains CRT reservation while adjacent committed pages preserve data");
        require(freeVirtual(splitHeap + 4096, 8192) == 0, "decommitting already reserved pages is idempotent");
        ctx.r3.u64 = splitHeap + 4096; ctx.r4.u64 = output; ctx.r5.u64 = 0;
        require(call("NtQueryVirtualMemory") == 0 && guest.read32(output) == splitHeap + 4096 &&
            guest.read32(output + 4) == splitHeap && guest.read32(output + 12) == 8192 &&
            guest.read32(output + 16) == MEM_RESERVE, "decommitted subregion reports original owner and reserved state");
        guest.commit(splitHeap + 4096, 8192);
        for (uint32_t index = 0; index < 8192; index += 4)
            require(guest.read32(splitHeap + 4096 + index) == 0, "recommitted pages discard previous contents and start zeroed");
        guest.write32(splitHeap + 4096, 0x11112222);
        guest.commit(splitHeap + 4096, 4096);
        require(guest.read32(splitHeap + 4096) == 0x11112222 && guest.read32(splitHeap) == 0x12345000 &&
            guest.read32(splitHeap + 16380) == 0xabcdef03, "commit on an already committed page retains data and neighbors");
        require(freeVirtual(splitHeap + 16383, 2) == 0xc000000d && guest.read32(freeBasePointer) == splitHeap + 16383 &&
            guest.read32(freeSizePointer) == 2 && guest.read32(splitHeap + 16380) == 0xabcdef03 &&
            guest.read32(adjacentHeap) == 0x87654321, "cross-reservation decommit rejects the whole request without mutation");
        require(freeVirtual(splitHeap, UINT32_MAX) == 0xc000000d && guest.valid(splitHeap, 16384),
            "oversized decommit cannot wrap the guest address space");
        require(freeVirtual(PPC_IMAGE_BASE, 4096) == 0xc000000d && guest.valid(PPC_IMAGE_BASE, 4096),
            "decommit cannot discard fixed executable image mappings");
        require(freeVirtual(startup, 4096) == 0xc000000d && guest.valid(startup, 4096),
            "virtual free cannot discard an owned physical codec buffer");
        guest.write32(freeBasePointer, splitHeap);
        ctx.r3.u64 = freeBasePointer; ctx.r4.u64 = 0x70000000; ctx.r5.u64 = MEM_DECOMMIT; ctx.r6.u64 = 0;
        require(call("NtFreeVirtualMemory") == 0xc0000005 && guest.valid(splitHeap, 16384),
            "inaccessible in/out parameter fails before changing page commitment");
        guest.write32(splitHeap + 8192, splitHeap + 8192); guest.write32(freeSizePointer, 4096);
        ctx.r3.u64 = splitHeap + 8192; ctx.r4.u64 = freeSizePointer; ctx.r5.u64 = MEM_DECOMMIT; ctx.r6.u64 = 0;
        require(call("NtFreeVirtualMemory") == 0xc0000005 && guest.valid(splitHeap, 16384) &&
            guest.read32(splitHeap + 8192) == splitHeap + 8192 && guest.read32(freeSizePointer) == 4096 &&
            guest.read32(splitHeap + 4096) == 0x11112222 && guest.read32(adjacentHeap) == 0x87654321,
            "decommit rejects an in/out address inside its rounded target before mutation");
        guest.write32(freeBasePointer, splitHeap); guest.write32(splitHeap + 8196, 0);
        ctx.r3.u64 = freeBasePointer; ctx.r4.u64 = splitHeap + 8196; ctx.r5.u64 = MEM_RELEASE; ctx.r6.u64 = 0;
        require(call("NtFreeVirtualMemory") == 0xc0000005 && guest.valid(splitHeap, 16384) &&
            guest.allocationSize(splitHeap) == 16384 && guest.read32(freeBasePointer) == splitHeap &&
            guest.read32(splitHeap + 8196) == 0 && guest.read32(adjacentHeap) == 0x87654321,
            "release rejects an in/out size inside its full reservation before mutation");
        const uint32_t largeVirtual = guest.reserve(3 * 65536, 65536, 65536);
        guest.commit(largeVirtual, 3 * 65536);
        guest.write32(largeVirtual, 0x55556666); guest.write32(largeVirtual + 2 * 65536, 0x77778888);
        require(freeVirtual(largeVirtual + 65536 + 123, 1) == 0 && guest.read32(freeBasePointer) == largeVirtual + 65536 &&
            guest.read32(freeSizePointer) == 65536 && !guest.valid(largeVirtual + 65536, 1) &&
            guest.read32(largeVirtual) == 0x55556666 && guest.read32(largeVirtual + 2 * 65536) == 0x77778888,
            "actual CRT large-page heap decommit uses owning 64 KiB granularity");
        guest.commit(largeVirtual + 65536, 65536);
        require(guest.read32(largeVirtual + 65536) == 0 && guest.read32(largeVirtual + 2 * 65536 - 4) == 0,
            "large-page recommit restores zeroed CPU backing");
        require(freeVirtual(splitHeap, 0, MEM_RELEASE) == 0 && guest.read32(freeBasePointer) == 0 &&
            guest.read32(freeSizePointer) == 0 && guest.allocationSize(splitHeap) == 0 && !guest.valid(splitHeap, 1),
            "release still removes the complete logical reservation");
        require(guest.release(adjacentHeap) && guest.release(largeVirtual), "release decommit contract reservations");
        ctx.r3.u64 = startup; ctx.r4.u64 = 0x70000000; ctx.r5.u64 = 0;
        require(call("NtQueryVirtualMemory") == 0xc0000005, "virtual query rejects inaccessible output");
        require(allocate(0) == 0 && allocate(0xffffffff) == 0 && allocate(4096, 4, 17) == 0, "invalid physical sizes and alignments fail without reservation");
        ctx.r3.u64 = 0; ctx.r4.u64 = startup + 4096;
        bool rejected = false;
        try { call("MmFreePhysicalMemory"); } catch (const NativeFailure&) { rejected = true; }
        require(rejected && guest.valid(startup, 0x2000000) && guest.read32(startup) == 0x12345678,
            "interior physical release cannot discard live parent buffer");
        ctx.r3.u64 = 0; ctx.r4.u64 = startup; call("MmFreePhysicalMemory");
        require(!guest.valid(startup, 1) && guest.allocationSize(startup) == 0, "physical release decommits owned CPU storage");
        ctx.r3.u64 = startup; ctx.r4.u64 = output; ctx.r5.u64 = 0;
        require(call("NtQueryVirtualMemory") == 0 && guest.read32(output + 16) == MEM_FREE, "released physical storage reports logically free");
        ctx.r3.u64 = 0; ctx.r4.u64 = largePage; call("MmFreePhysicalMemory");
        require(guest.release(virtualReservation), "release virtual contract reservation");
        currentContext = nullptr; memory = nullptr;
        fprintf(stderr, "Native reservation transactions, physical CPU buffers, page/cache ABI, bounded ownership, virtual query, decommit/recommit and release contracts passed.\n");
        return 0;
    } catch (const std::exception& error) { fprintf(stderr, "Memory service contract: %s\n", error.what()); return 1; }
}
