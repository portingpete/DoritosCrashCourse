#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include "ppc_context.h"
#include "xex_loader.h"

namespace Doritos::Native {
using MmioReadHandler = bool (*)(uint32_t, unsigned, uint64_t&);
using MmioWriteHandler = bool (*)(uint32_t, uint64_t, unsigned);
// Install before guest workers start; clear only after they have stopped.
void setNativeMmioHandlers(MmioReadHandler read, MmioWriteHandler write);
struct MemoryQueryCounters {
    uint64_t validations = 0, queries = 0, queryNanoseconds = 0, validationNanoseconds = 0;
};
// Opt-in cumulative counters for the calling native thread; no memory caching.
bool memoryQueryTimingEnabled() noexcept;
MemoryQueryCounters memoryQueryCounters() noexcept;
struct CriticalMemoryQueryCounters {
    uint64_t calls = 0;
    MemoryQueryCounters queries;
};
CriticalMemoryQueryCounters criticalMemoryQueryCounters() noexcept;
void recordCriticalMemoryQueries(const MemoryQueryCounters& before) noexcept;
class NativeFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class Memory {
public:
    explicit Memory(const LoadedXex& image);
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    uint8_t* base() const { return base_; }
    uint32_t allocate(uint32_t size, uint32_t alignment = 4096);
    uint32_t reserve(uint32_t size, uint32_t alignment = 4096, uint32_t pageSize = 4096);
    uint32_t reserveRange(uint32_t size, uint32_t alignment, uint32_t pageSize, uint32_t begin, uint64_t endExclusive);
    std::map<uint32_t, uint32_t> reservationSnapshot() const;
    bool reservationContains(uint32_t address, uint32_t bytes) const;
    uint32_t allocationSize(uint32_t address) const;
    uint32_t reservationPageSize(uint32_t address) const;
    // Remove page backing while keeping the logical guest reservation owned.
    bool decommit(uint32_t address, uint32_t size);
    bool release(uint32_t address);
    void commit(uint32_t address, uint32_t size);
    void initializeThread(PPCContext& ctx, uint32_t stackSize);
    void initializeDataImports(const LoadedXex& image);
    uint32_t read32(uint32_t address) const;
    void write32(uint32_t address, uint32_t value);
    uint64_t read64(uint32_t address) const;
    void write64(uint32_t address, uint64_t value);
    bool valid(uint32_t address, uint32_t bytes) const;
    // Fresh joint proof, with the existing scalar policy for uncertain pages.
    // Consume both spans immediately, before guest calls or memory mutations.
    bool validPair(uint32_t firstAddress, uint32_t firstBytes, uint32_t secondAddress, uint32_t secondBytes) const;
    std::string unsupportedDataImport(uint32_t address) const;
private:
    uint8_t* base_ = nullptr;
    uint32_t nextAllocation_ = 0x10000000;
    std::map<uint32_t, uint32_t> allocations_;
    std::map<uint32_t, uint32_t> fixedMappings_;
    std::map<uint32_t, uint32_t> allocationPages_;
    uint64_t reservationsCreated_ = 0, reservationsReleased_ = 0, cumulativeReservedBytes_ = 0;
    std::map<uint32_t, std::string> unsupportedDataImports_;
    std::jthread timestampThread_;
    mutable std::recursive_mutex mutex_;
};
extern Memory* memory;
extern thread_local PPCContext* currentContext;
int runGuest(PPCContext& ctx, uint8_t* base, PPCFunc* entry);
LONG exceptionFilter(EXCEPTION_POINTERS* exception);
[[noreturn]] void fail(const char* reason);
void printContext(const PPCContext& ctx);
// Opt-in read-only samples of original game state; no guest calls or writes.
void reportOriginalGameState();
}
