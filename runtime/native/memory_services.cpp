#include "runtime.h"
#include "memory_services.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <string_view>

namespace Doritos::Native {
namespace {
constexpr uint32_t invalidParameter = 0xc000000d, accessViolation = 0xc0000005;
constexpr uint32_t physicalBytes = 0x20000000;
struct PhysicalAllocation {
    uint32_t virtualBase, physicalBase, size, page, protection, flags;
};
std::mutex physicalMutex;
std::map<uint32_t, PhysicalAllocation> physical;
Memory* owner = nullptr;
uint64_t alignUp(uint64_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(uint64_t(alignment) - 1);
}
void initialized() {
    if (!memory || owner != memory) fail("Native memory services were not initialized for this guest memory");
}
bool writable(uint32_t address, uint32_t size) {
    if (!memory->valid(address, size)) return false;
    uint64_t cursor = address, end = cursor + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(memory->base() + cursor, &info, sizeof(info))) return false;
        const DWORD protection = info.Protect & 0xff;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const uint64_t next = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize - memory->base();
        if (next <= cursor) return false;
        cursor = std::min(next, end);
    }
    return true;
}
auto containing(uint32_t address) {
    auto found = physical.upper_bound(address);
    if (found == physical.begin()) return physical.end();
    --found;
    return uint64_t(address) < uint64_t(found->second.virtualBase) + found->second.size ? found : physical.end();
}
uint32_t allocatePhysical(PPCContext& ctx) {
    const uint32_t flags = ctx.r3.u32, size = ctx.r4.u32, protection = ctx.r5.u32;
    const uint32_t minimum = ctx.r6.u32, maximum = ctx.r7.u32;
    uint32_t alignment = ctx.r8.u32;
    // Proven original callers use process classes 0 and 2 and read/write
    // protection, with optional cache flags and Xbox page-size bits.
    if (flags != 0 && flags != 2) fail("Physical allocation process class requires a verified native contract");
    if ((protection & 0xff) != PAGE_READWRITE || (protection & ~0xa0000604u) ||
        (protection & 0xa0000000u) == 0xa0000000u)
        fail("Physical allocation protection requires a verified native contract");
    if (minimum != 0 || maximum != 0xffffffff) fail("Physical allocation bounds require a verified native contract");
    const uint32_t page = (protection & 0x20000000) ? 65536 : (protection & 0x80000000) ? 0x1000000 : 4096;
    if (!alignment) alignment = page;
    if (alignment & (alignment - 1)) return 0;
    alignment = std::max(alignment, page);
    const uint64_t rounded = alignUp(size, page);
    if (!size || rounded > physicalBytes || alignment > physicalBytes) return 0;
    const uint32_t region = page == 65536 ? 0xa0000000 : page == 0x1000000 ? 0xc0000000 : 0xe0000000;
    // The E-region's 4 KiB virtual view has a one-page physical bias. Keep
    // common physical offsets unique across all three allocation regions;
    // honoring the requested virtual alignment may leave a physical gap.
    const uint32_t bias = region == 0xe0000000 ? 4096 : 0;
    std::lock_guard lock(physicalMutex);
    uint64_t position = 0x10000;
    std::map<uint32_t, uint32_t> occupied;
    for (const auto& [address, allocation] : physical) occupied.emplace(allocation.physicalBase, allocation.size);
    auto virtualFor = [&](uint64_t offset) { return alignUp(uint64_t(region) + offset - bias, alignment); };
    uint64_t address = virtualFor(position), offset = address - region + bias;
    for (const auto& [start, bytes] : occupied) {
        if (uint64_t(start) + bytes <= offset) continue;
        if (offset + rounded <= start) break;
        address = virtualFor(uint64_t(start) + bytes);
        offset = address - region + bias;
    }
    if (offset + rounded > physicalBytes || address + rounded > 0xfffff000ull) return 0;
    const uint32_t allocation = memory->reserveRange(uint32_t(rounded), alignment, page,
        uint32_t(address), address + rounded);
    if (!allocation) return 0;
    try { memory->commit(allocation, uint32_t(rounded)); }
    catch (...) { memory->release(allocation); throw; }
    physical.emplace(allocation, PhysicalAllocation{allocation, uint32_t(offset), uint32_t(rounded), page, protection, flags});
    fprintf(stderr, "[MEMORY] owned CPU physical buffer virtual=0x%08X physical=0x%08X bytes=0x%X page=0x%X\n",
        allocation, uint32_t(offset), uint32_t(rounded), page);
    return allocation;
}
uint32_t allocateVirtual(PPCContext& ctx) {
    // Xbox ABI: in/out base DWORD, in/out size DWORD, allocation type,
    // protection, devkit-memory flag. Original CRT 823E76A0 adds NOZERO
    // when HEAP_ZERO_MEMORY is absent and commits a new null-base buffer.
    const uint32_t addressPointer = ctx.r3.u32, sizePointer = ctx.r4.u32;
    const uint32_t allocationType = ctx.r5.u32, protection = ctx.r6.u32;
    constexpr uint32_t noZero = 0x00800000, largePages = 0x20000000, heap = 0x40000000;
    constexpr uint32_t allowedFlags = MEM_RESERVE | MEM_COMMIT | noZero | largePages | heap;
    if (!(allocationType & (MEM_RESERVE | MEM_COMMIT)) || (allocationType & ~allowedFlags) || protection != PAGE_READWRITE)
        fail("Unsupported NtAllocateVirtualMemory flags/protection");
    if (ctx.r7.u32) fail("Virtual allocation devkit memory requires a verified native contract");
    if (!writable(addressPointer, 4) || !writable(sizePointer, 4)) return accessViolation;
    uint32_t requested = memory->read32(addressPointer), bytes = memory->read32(sizePointer);
    if (!bytes) return invalidParameter;
    uint32_t page = (allocationType & largePages) ? 65536u : 4096u;
    uint64_t rounded = alignUp(bytes, page);
    if (rounded > UINT32_MAX) return 0xc0000017;
    bool created = false;
    if ((allocationType & MEM_RESERVE) || !requested) {
        if (requested) fail("Unsupported fixed virtual reservation");
        requested = memory->reserve(uint32_t(rounded), 65536u, page);
        rounded = memory->allocationSize(requested);
        created = true;
    } else {
        {
            std::lock_guard lock(physicalMutex);
            if (containing(requested) != physical.end()) return invalidParameter;
        }
        page = memory->reservationPageSize(requested);
        if (!page) fail("Virtual commit has no owning reservation");
        const uint32_t aligned = requested & ~(page - 1);
        rounded = alignUp(uint64_t(requested - aligned) + bytes, page);
        requested = aligned;
    }
    if (rounded > UINT32_MAX || !memory->reservationContains(requested, uint32_t(rounded)))
        fail("Virtual commit exceeds owned native reservation");
    if (allocationType & MEM_COMMIT) {
        // NOZERO permits unspecified initial contents; native Windows pages
        // may still be zero. Never clear already committed heap data here.
        try { memory->commit(requested, uint32_t(rounded)); }
        catch (...) { if (created) memory->release(requested); throw; }
    }
    memory->write32(addressPointer, requested); memory->write32(sizePointer, uint32_t(rounded));
    return 0;
}
uint32_t queryVirtual(uint32_t address, uint32_t output, uint32_t flags) {
    // The Xbox import takes address, fixed-size output, process flags. It does
    // not use the Windows NtQueryVirtualMemory process/class/length ABI.
    if (flags != 0 && flags != 3) fail("Virtual query process flags require a verified native contract");
    if (!output) return invalidParameter;
    if (!writable(output, 28)) return accessViolation;
    MEMORY_BASIC_INFORMATION host{};
    if (!VirtualQuery(memory->base() + address, &host, sizeof(host))) return invalidParameter;
    const auto reservations = memory->reservationSnapshot();
    auto reservation = reservations.upper_bound(address);
    if (reservation != reservations.begin()) {
        auto previous = std::prev(reservation);
        if (uint64_t(address) < uint64_t(previous->first) + previous->second) reservation = previous;
    }
    uint32_t regionBase = address & ~4095u, allocationBase = 0, allocationProtect = 0, protect = 0, state = MEM_FREE, type = 0;
    uint64_t regionEnd = 0x100000000ull;
    if (reservation != reservations.end() && reservation->first <= address) {
        allocationBase = reservation->first;
        allocationProtect = PAGE_READWRITE;
        regionBase = std::max(allocationBase, uint32_t(static_cast<uint8_t*>(host.BaseAddress) - memory->base()));
        regionEnd = std::min(uint64_t(allocationBase) + reservation->second,
            uint64_t(static_cast<uint8_t*>(host.BaseAddress) - memory->base()) + host.RegionSize);
        state = host.State == MEM_COMMIT ? MEM_COMMIT : MEM_RESERVE;
        protect = state == MEM_COMMIT ? host.Protect : 0;
        type = MEM_PRIVATE;
        std::lock_guard lock(physicalMutex);
        auto physicalRegion = containing(address);
        if (physicalRegion != physical.end()) {
            allocationProtect = physicalRegion->second.protection;
            if (state == MEM_COMMIT) protect = physicalRegion->second.protection;
        }
    } else if (host.State == MEM_COMMIT) {
        regionBase = uint32_t(static_cast<uint8_t*>(host.BaseAddress) - memory->base());
        regionEnd = uint64_t(regionBase) + host.RegionSize;
        allocationBase = regionBase;
        allocationProtect = host.Protect;
        protect = host.Protect; state = MEM_COMMIT; type = MEM_PRIVATE;
        if (address >= PPC_IMAGE_BASE && uint64_t(address) < uint64_t(PPC_IMAGE_BASE) + PPC_IMAGE_SIZE) {
            allocationBase = PPC_IMAGE_BASE; type = MEM_IMAGE;
        }
    } else {
        // Host reservation covers the entire 4 GiB span. Report logical guest
        // free space only to the next owned region or committed host region.
        if (reservation != reservations.end()) regionEnd = reservation->first;
        uint64_t cursor = regionBase;
        while (cursor < regionEnd) {
            MEMORY_BASIC_INFORMATION next{};
            if (!VirtualQuery(memory->base() + cursor, &next, sizeof(next))) break;
            const uint64_t nextBase = static_cast<uint8_t*>(next.BaseAddress) - memory->base();
            if (next.State == MEM_COMMIT) { regionEnd = std::min(regionEnd, nextBase); break; }
            const uint64_t nextEnd = nextBase + next.RegionSize;
            if (nextEnd <= cursor) fail("Native virtual query failed to advance");
            cursor = std::min(nextEnd, regionEnd);
        }
    }
    if (regionEnd <= regionBase || regionEnd - regionBase > UINT32_MAX) return invalidParameter;
    const uint32_t values[]{regionBase, allocationBase, allocationProtect, uint32_t(regionEnd - regionBase), state, protect, type};
    for (unsigned i = 0; i < 7; ++i) memory->write32(output + 4 * i, values[i]);
    return 0;
}
uint32_t freeVirtual(PPCContext& ctx) {
    // Original CRT 0x823E65F8 uses r3/r4 as in/out DWORD pointers, r5
    // MEM_DECOMMIT and r6=0, then records the resulting retained heap range.
    const uint32_t addressPointer = ctx.r3.u32, sizePointer = ctx.r4.u32;
    if (ctx.r6.u32 != 0) fail("Virtual free devkit memory requires a verified native contract");
    if (ctx.r5.u32 != MEM_DECOMMIT && ctx.r5.u32 != MEM_RELEASE)
        fail("Unsupported NtFreeVirtualMemory mode");
    if (!writable(addressPointer, 4) || !writable(sizePointer, 4)) return accessViolation;
    uint32_t address = memory->read32(addressPointer), bytes = memory->read32(sizePointer);
    {
        std::lock_guard lock(physicalMutex);
        if (containing(address) != physical.end()) return invalidParameter;
    }
    auto containsParameter = [&](uint32_t base, uint32_t size) {
        const uint64_t end = uint64_t(base) + size;
        auto overlaps = [&](uint32_t pointer) { return uint64_t(pointer) < end && uint64_t(pointer) + 4 > base; };
        return overlaps(addressPointer) || overlaps(sizePointer);
    };
    if (ctx.r5.u32 == MEM_RELEASE) {
        const uint32_t size = memory->allocationSize(address);
        if (!size) return invalidParameter;
        if (containsParameter(address, size)) return accessViolation;
        if (!memory->release(address)) return invalidParameter;
        memory->write32(addressPointer, 0); memory->write32(sizePointer, 0);
        return 0;
    }
    const uint32_t page = memory->reservationPageSize(address);
    if (!bytes || !page) return invalidParameter;
    const uint32_t aligned = address & ~(page - 1);
    const uint64_t rounded = alignUp(uint64_t(address - aligned) + bytes, page);
    if (rounded > UINT32_MAX || !memory->reservationContains(aligned, uint32_t(rounded))) return invalidParameter;
    // Capture both writable results before changing backing. Guest metadata
    // inside this range would become inaccessible before the output writes.
    if (containsParameter(aligned, uint32_t(rounded))) return accessViolation;
    if (!memory->decommit(aligned, uint32_t(rounded))) return invalidParameter;
    memory->write32(addressPointer, aligned); memory->write32(sizePointer, uint32_t(rounded));
    return 0;
}
}
void initializeMemoryServices() {
    if (!memory) fail("Native memory services require initialized guest memory");
    std::lock_guard lock(physicalMutex);
    if (owner == memory && !physical.empty()) fail("Cannot reset live native physical allocations");
    physical.clear(); owner = memory;
}
uint32_t resolvePhysicalAddress(uint32_t offset, uint32_t size) {
    initialized();
    if (!size || uint64_t(offset) + size > physicalBytes) return 0;
    std::lock_guard lock(physicalMutex);
    for (const auto& [address, allocation] : physical) {
        if (offset < allocation.physicalBase || uint64_t(offset) + size > uint64_t(allocation.physicalBase) + allocation.size) continue;
        const uint32_t mapped = address + offset - allocation.physicalBase;
        return memory->valid(mapped, size) ? mapped : 0;
    }
    return 0;
}
bool memoryServicesImport(PPCContext& ctx, uint8_t*, const char* name) {
    const std::string_view function(name);
    if (function != "MmAllocatePhysicalMemoryEx" && function != "MmFreePhysicalMemory" && function != "MmGetPhysicalAddress" &&
        function != "MmQueryAllocationSize" && function != "MmQueryAddressProtect" && function != "NtQueryVirtualMemory" &&
        function != "NtFreeVirtualMemory" && function != "NtAllocateVirtualMemory" && function != "MmMapIoSpace") return false;
    initialized();
    auto result = [&](uint32_t value) { ctx.r3.u64 = value; return true; };
    if (function == "MmAllocatePhysicalMemoryEx") return result(allocatePhysical(ctx));
    if (function == "NtAllocateVirtualMemory") return result(allocateVirtual(ctx));
    if (function == "MmFreePhysicalMemory") {
        const uint32_t flags = ctx.r3.u32, address = ctx.r4.u32;
        if (!address) return true;
        std::lock_guard lock(physicalMutex);
        auto allocation = physical.find(address);
        if (allocation == physical.end() || allocation->second.flags != flags) fail("Physical release does not match an owned allocation base/process class");
        if (!memory->release(address)) fail("Native physical reservation was lost before release");
        physical.erase(allocation); return true;
    }
    if (function == "MmGetPhysicalAddress") {
        std::lock_guard lock(physicalMutex);
        const uint32_t address = ctx.r3.u32;
        auto allocation = containing(address);
        if (allocation == physical.end()) fail("Physical address query requires an owned CPU physical buffer");
        ctx.r4.u64 = 0;
        return result(allocation->second.physicalBase + address - allocation->second.virtualBase);
    }
    if (function == "MmQueryAllocationSize") return result(memory->allocationSize(ctx.r3.u32));
    if (function == "MmQueryAddressProtect") {
        std::lock_guard lock(physicalMutex);
        auto allocation = containing(ctx.r3.u32);
        if (allocation != physical.end()) return result(allocation->second.protection);
        if (!memory->valid(ctx.r3.u32, 1)) return result(0);
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(memory->base() + ctx.r3.u32, &info, sizeof(info))) return result(0);
        return result(info.Protect);
    }
    if (function == "NtQueryVirtualMemory") return result(queryVirtual(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32));
    if (function == "NtFreeVirtualMemory") return result(freeVirtual(ctx));
    if (function == "MmMapIoSpace") {
        // Original XMA SDK maps a 64-byte context with process class2 and
        // PAGE_READWRITE|NOCACHE. Native decoding uses coherent CPU backing;
        // the existing owned virtual address is the actual mapped storage.
        if (ctx.r3.u32 != 2 || ctx.r6.u32 != 0x404)
            fail("Native physical mapping requires a verified owned CPU contract");
        const uint32_t address = resolvePhysicalAddress(ctx.r4.u32, ctx.r5.u32);
        if (!address) fail("Native physical mapping does not reference an owned CPU buffer");
        return result(address);
    }
    return false;
}
}
