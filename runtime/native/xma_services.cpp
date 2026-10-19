// Native XMA manager state for the original SDK's control/context operations.
// Original helpers: pool discovery 82607388, allocation 826073E8, poll 82607C10,
// submission 82607D80. All 133 original audio assets are Vorbis/IMA ADPCM/PCM;
// compressed XMA execution remains an explicit boundary until it is required.
#include "runtime.h"
#include "memory_services.h"
#include "kernel_objects.h"
#include "xma_services.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>

namespace Doritos::Native {
namespace {
constexpr uint32_t contextCount = 320, contextBytes = 64;
constexpr uint32_t registerBase = 0x7fea0000;
struct Context { bool owned = false, queued = false, locked = false; };
std::mutex engineMutex;
Memory* owner = nullptr;
uint32_t pool = 0, poolPhysical = 0;
bool paused = false;
std::array<Context, contextCount> contexts{};
void initialized() {
    if (!memory) fail("Native XMA manager requires guest memory");
    if (!owner) owner = memory;
    if (owner != memory) fail("Native XMA state belongs to another guest memory");
}
bool writable(uint32_t address, uint32_t bytes) {
    if (!memory->valid(address, bytes)) return false;
    uint64_t cursor = address, end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(memory->base() + cursor, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD)) return false;
        const DWORD protection = info.Protect & 255;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const uint64_t next = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize - memory->base();
        if (next <= cursor) return false;
        cursor = next < end ? next : end;
    }
    return true;
}
uint32_t ensurePool() {
    if (pool) return pool;
    PPCContext allocation{};
    allocation.r3.u64 = 2; allocation.r4.u64 = contextCount * contextBytes;
    allocation.r5.u64 = 0x404; allocation.r6.u64 = 0; allocation.r7.u64 = 0xffffffff;
    allocation.r8.u64 = 4096;
    memoryServicesImport(allocation, memory->base(), "MmAllocatePhysicalMemoryEx");
    pool = allocation.r3.u32;
    if (!pool) return 0;
    allocation.r3.u64 = pool;
    memoryServicesImport(allocation, memory->base(), "MmGetPhysicalAddress");
    poolPhysical = allocation.r3.u32;
    fprintf(stderr, "[XMA] Owned native context storage virtual=0x%08X physical=0x%08X contexts=%u\n", pool, poolPhysical, contextCount);
    return pool;
}
uint32_t slotFor(uint32_t address) {
    if (!pool || address < pool || uint64_t(address) >= uint64_t(pool) + contextCount * contextBytes || (address - pool) % contextBytes)
        return contextCount;
    return (address - pool) / contextBytes;
}
void completeEmptyWork(uint32_t slot) {
    // There is no input to consume and no PCM to publish for an empty manager
    // job. Preserve every guest context field. Accepting compressed work here
    // would otherwise let the game mistake missing decoding for completion.
    const uint32_t address = pool + slot * contextBytes;
    if (memory->read32(address) & 0x00300000) {
        fprintf(stderr, "[XMA] Compressed input requested by context %u at 0x%08X\n", slot, address);
        fail("Native compressed XMA decoding has not been implemented");
    }
    contexts[slot].queued = false;
}
void contextCommand(uint32_t first, uint32_t mask, uint32_t command) {
    for (uint32_t bit = 0; bit < 32; ++bit) {
        if (!(mask & (uint32_t(1) << bit))) continue;
        const uint32_t slot = first + bit;
        if (slot >= contextCount || !contexts[slot].owned) fail("Native XMA command references an unowned context");
        auto& context = contexts[slot];
        if (command == 0x1940) {
            context.locked = false;
            // Validate before accepting work; failure cannot publish completion.
            if (memory->read32(pool + slot * contextBytes) & 0x00300000)
                fail("Native compressed XMA decoding has not been implemented");
            context.queued = true;
            if (!paused) completeEmptyWork(slot);
        } else if (command == 0x1a40) {
            if (context.queued) completeEmptyWork(slot);
            context.locked = true;
        } else {
            // Clear resets the native decoder job/history, never manufactures
            // buffer contents or modifies the SDK's staged context image.
            if (memory->read32(pool + slot * contextBytes) & 0x00300000)
                fail("Clearing active compressed XMA work requires a native decoder");
            context.queued = false; context.locked = false;
        }
    }
}
}
bool xmaServicesImport(PPCContext& ctx, uint8_t*, const char* name) {
    const std::string_view function(name);
    if (function != "XMACreateContext" && function != "XMAReleaseContext") return false;
    std::lock_guard lock(engineMutex); initialized();
    if (function == "XMACreateContext") {
        const uint32_t output = ctx.r3.u32;
        if (!writable(output, 4)) { ctx.r3.u64 = 0xc000000d; return true; }
        if (!ensurePool()) { ctx.r3.u64 = 0xc0000017; return true; }
        for (uint32_t slot = 0; slot < contextCount; ++slot) {
            if (contexts[slot].owned) continue;
            contexts[slot] = {true, false, false};
            const uint32_t address = pool + slot * contextBytes;
            memset(memory->base() + address, 0, contextBytes);
            memory->write32(output, address); ctx.r3.u64 = 0; return true;
        }
        memory->write32(output, 0); ctx.r3.u64 = 0xc0000017; return true;
    }
    const uint32_t slot = slotFor(ctx.r3.u32);
    if (slot == contextCount || !contexts[slot].owned) { ctx.r3.u64 = 0xc000000d; return true; }
    contexts[slot] = {};
    memset(memory->base() + pool + slot * contextBytes, 0, contextBytes);
    ctx.r3.u64 = 0; return true;
}
bool xmaReadMmio(uint32_t address, unsigned width, uint64_t& value) {
    if ((address & 0xffff0000) != registerBase) return false;
    if (width != 4 || (address & 3)) fail("Native XMA register access requires a verified 32-bit contract");
    std::lock_guard lock(engineMutex); initialized();
    const uint32_t offset = address - registerBase;
    if (offset == 0x1800) {
        if (!ensurePool()) fail("Native XMA context storage allocation failed");
        value = _byteswap_ulong(poolPhysical); return true;
    }
    fail("Native XMA register read requires a verified operation");
}
bool xmaWriteMmio(uint32_t address, uint64_t value, unsigned width) {
    if ((address & 0xffff0000) != registerBase) return false;
    if (width != 4 || (address & 3) || value > UINT32_MAX) fail("Native XMA register access requires a verified 32-bit contract");
    std::lock_guard lock(engineMutex); initialized();
    const uint32_t offset = address - registerBase, command = _byteswap_ulong(uint32_t(value));
    if (offset == 0x1804) {
        if (command == 2) { paused = true; return true; }
        if (command == 3) {
            for (uint32_t slot = 0; slot < contextCount; ++slot)
                if (contexts[slot].queued) completeEmptyWork(slot);
            paused = false; return true;
        }
        fail("Native XMA control value requires a verified operation");
    }
    for (const uint32_t group : {0x1940u, 0x1a40u, 0x1a80u}) {
        if (offset < group || offset >= group + 40) continue;
        contextCommand((offset - group) * 8, command, group); return true;
    }
    fail("Native XMA register write requires a verified operation");
}
void shutdownXmaServices(Memory* guest) {
    std::lock_guard lock(engineMutex);
    if (owner != guest || liveGuestThreads()) return;
    if (pool) {
        PPCContext release{}; release.r3.u64 = 2; release.r4.u64 = pool;
        memoryServicesImport(release, guest->base(), "MmFreePhysicalMemory");
    }
    contexts = {}; owner = nullptr; pool = poolPhysical = 0; paused = false;
}
}
