#include "runtime.h"
#include "environment.h"
#include "kernel_objects.h"
#include "filesystem.h"
#include "memory_services.h"
#include "audio_services.h"
#include "xma_services.h"
#include "input_services.h"
#include "user_services.h"
#include "network_services.h"
#include "import_trace_key.h"
#include "math.h"
#include <xinput.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <string_view>

namespace {
using namespace Doritos::Native;
std::mutex stateMutex;
std::map<uint32_t, std::shared_ptr<std::recursive_mutex>> criticalSections;
std::map<uint32_t, std::shared_ptr<std::mutex>> spinLocks;
bool tlsAllocated[64]{};
std::set<std::string, ImportTraceKeyLess> tracedImports;
thread_local uint32_t irql = 0;
struct CriticalQuerySite {
    uint32_t lr = 0, firstAddress = 0, firstPCR = 0;
    CriticalSectionOperation operation = CriticalSectionOperation::Enter;
    bool mixedAddresses = false, mixedPCRs = false;
    uint64_t calls = 0;
    MemoryQueryCounters queries;
};
struct CriticalQuerySites {
    std::array<CriticalQuerySite, 256> sites{};
    CriticalQuerySite overflow{};
    unsigned used = 0;
    uint64_t operations = 0, reports = 0;
};
thread_local CriticalQuerySites criticalQuerySites;
const char* criticalOperationName(CriticalSectionOperation operation) noexcept {
    switch (operation) {
    case CriticalSectionOperation::Enter: return "enter";
    case CriticalSectionOperation::TryEnter: return "try_enter";
    case CriticalSectionOperation::Leave: return "leave";
    }
    return "unknown";
}
void recordCriticalQuerySite(uint32_t lr, uint32_t address, uint32_t pcr,
    CriticalSectionOperation operation, const MemoryQueryCounters& before) noexcept {
    const auto after = memoryQueryCounters();
    auto& batch = criticalQuerySites;
    CriticalQuerySite* site = nullptr;
    for (unsigned index = 0; index < batch.used; ++index) {
        auto& candidate = batch.sites[index];
        if (candidate.lr == lr && candidate.operation == operation) { site = &candidate; break; }
    }
    if (!site && batch.used < batch.sites.size()) {
        site = &batch.sites[batch.used++];
        site->lr = lr; site->operation = operation;
    }
    if (!site) site = &batch.overflow;
    if (!site->calls) { site->firstAddress = address; site->firstPCR = pcr; }
    else {
        site->mixedAddresses |= site->firstAddress != address;
        site->mixedPCRs |= site->firstPCR != pcr;
    }
    ++site->calls;
    site->queries.validations += after.validations - before.validations;
    site->queries.queries += after.queries - before.queries;
    site->queries.queryNanoseconds += after.queryNanoseconds - before.queryNanoseconds;
    site->queries.validationNanoseconds += after.validationNanoseconds - before.validationNanoseconds;
    if (++batch.operations != 65536) return;

    // Reuse query counters only: no extra clock reads or guest memory access.
    // Reports cover this host thread's operations, not a present/frame interval.
    const DWORD lastError = GetLastError();
    std::array<const CriticalQuerySite*, 256> ranked{};
    for (unsigned index = 0; index < batch.used; ++index) ranked[index] = &batch.sites[index];
    std::sort(ranked.begin(), ranked.begin() + batch.used, [](const auto* left, const auto* right) {
        if (left->queries.queryNanoseconds != right->queries.queryNanoseconds)
            return left->queries.queryNanoseconds > right->queries.queryNanoseconds;
        return left->calls > right->calls;
    });
    const DWORD thread = GetCurrentThreadId();
    const uint64_t report = ++batch.reports;
    std::fprintf(stderr, "[CRITICAL QUERY CALLERS] thread=%lu batch=%llu operations=%llu sites=%u "
        "overflow_calls=%llu top_limit=8 sort=query_nanoseconds scope=current_thread_enter_try_leave "
        "query_subset_of_whole_queries=true query_subset_of_enclosing_import=true "
        "not_additive_to_import_queries=true lock_wait_measured=false frame_interval_measured=false\n",
        thread, report, batch.operations, batch.used, batch.overflow.calls);
    const auto print = [&](const CriticalQuerySite& entry, bool overflow) {
        std::fprintf(stderr, "[CRITICAL QUERY CALLER] thread=%lu batch=%llu op=%s lr=%08X calls=%llu "
            "queries=%llu query_ms=%.3f validation_ms=%.3f queries_per_call=%.3f "
            "first_address=%08X mixed_addresses=%u first_pcr=%08X mixed_pcrs=%u "
            "overflow_bucket=%u scope=current_thread_operation_batch query_subset_of_whole_queries=true "
            "query_subset_of_enclosing_import=true not_additive_to_import_queries=true "
            "lock_wait_measured=false frame_interval_measured=false\n",
            thread, report, overflow ? "all_untracked" : criticalOperationName(entry.operation),
            overflow ? 0u : entry.lr, entry.calls, entry.queries.queries,
            double(entry.queries.queryNanoseconds) / 1e6, double(entry.queries.validationNanoseconds) / 1e6,
            double(entry.queries.queries) / double(entry.calls), entry.firstAddress,
            unsigned(entry.mixedAddresses), entry.firstPCR, unsigned(entry.mixedPCRs), unsigned(overflow));
    };
    for (unsigned index = 0; index < std::min(batch.used, 8u); ++index) print(*ranked[index], false);
    if (batch.overflow.calls) print(batch.overflow, true);
    batch.used = 0; batch.operations = 0;
    batch.sites = {}; batch.overflow = {};
    SetLastError(lastError);
}
class CriticalMemoryQueryScope {
    bool enabled_;
    MemoryQueryCounters before_;
    uint32_t lr_ = 0, address_ = 0, pcr_ = 0;
    CriticalSectionOperation operation_;
public:
    CriticalMemoryQueryScope(const PPCContext& ctx, CriticalSectionOperation operation) :
        enabled_(memoryQueryTimingEnabled()), before_(enabled_ ? memoryQueryCounters() : MemoryQueryCounters{}),
        operation_(operation) {
        if (enabled_) { lr_ = uint32_t(ctx.lr); address_ = ctx.r3.u32; pcr_ = ctx.r13.u32; }
    }
    ~CriticalMemoryQueryScope() {
        if (enabled_) {
            recordCriticalMemoryQueries(before_);
            recordCriticalQuerySite(lr_, address_, pcr_, operation_, before_);
        }
    }
};
struct ImportQuerySite {
    uint32_t lr = 0, ordinal = 0;
    const char* library = nullptr;
    const char* name = nullptr;
    uint64_t calls = 0;
    MemoryQueryCounters queries;
};
struct ImportQuerySites {
    std::array<ImportQuerySite, 256> sites{};
    ImportQuerySite overflow{};
    unsigned used = 0;
    uint64_t operations = 0, reports = 0;
};
thread_local ImportQuerySites importQuerySites;
void recordImportQuerySite(uint32_t lr, uint32_t ordinal, const char* library, const char* name,
    const MemoryQueryCounters& before) noexcept {
    const auto after = memoryQueryCounters();
    auto& batch = importQuerySites;
    ImportQuerySite* site = nullptr;
    for (unsigned index = 0; index < batch.used; ++index) {
        auto& candidate = batch.sites[index];
        if (candidate.lr == lr && candidate.ordinal == ordinal && candidate.library == library && candidate.name == name) {
            site = &candidate; break;
        }
    }
    if (!site && batch.used < batch.sites.size()) {
        site = &batch.sites[batch.used++];
        site->lr = lr; site->ordinal = ordinal; site->library = library; site->name = name;
    }
    if (!site) site = &batch.overflow;
    ++site->calls;
    site->queries.validations += after.validations - before.validations;
    site->queries.queries += after.queries - before.queries;
    site->queries.queryNanoseconds += after.queryNanoseconds - before.queryNanoseconds;
    site->queries.validationNanoseconds += after.validationNanoseconds - before.validationNanoseconds;
    if (++batch.operations != 65536) return;

    const DWORD lastError = GetLastError();
    std::array<const ImportQuerySite*, 256> ranked{};
    for (unsigned index = 0; index < batch.used; ++index) ranked[index] = &batch.sites[index];
    std::sort(ranked.begin(), ranked.begin() + batch.used, [](const auto* left, const auto* right) {
        if (left->queries.queryNanoseconds != right->queries.queryNanoseconds)
            return left->queries.queryNanoseconds > right->queries.queryNanoseconds;
        return left->calls > right->calls;
    });
    const DWORD thread = GetCurrentThreadId();
    const uint64_t report = ++batch.reports;
    std::fprintf(stderr, "[IMPORT QUERY CALLERS] thread=%lu batch=%llu imports=%llu sites=%u "
        "overflow_calls=%llu top_limit=8 sort=query_nanoseconds scope=current_thread_inclusive_imports "
        "nested_calls_may_overlap=true critical_queries_overlap_imports=true "
        "not_additive_to_whole_queries=true lock_wait_measured=false frame_interval_measured=false\n",
        thread, report, batch.operations, batch.used, batch.overflow.calls);
    const auto print = [&](const ImportQuerySite& entry, bool overflow) {
        std::fprintf(stderr, "[IMPORT QUERY CALLER] thread=%lu batch=%llu library=\"%s\" name=\"%s\" "
            "ordinal=%u lr=%08X calls=%llu queries=%llu query_ms=%.3f validation_ms=%.3f "
            "queries_per_call=%.3f overflow_bucket=%u scope=current_thread_inclusive_import_batch "
            "nested_calls_may_overlap=true critical_queries_overlap_imports=true "
            "not_additive_to_whole_queries=true lock_wait_measured=false frame_interval_measured=false\n",
            thread, report, overflow ? "all_untracked" : entry.library, overflow ? "all_untracked" : entry.name,
            overflow ? 0u : entry.ordinal, overflow ? 0u : entry.lr, entry.calls, entry.queries.queries,
            double(entry.queries.queryNanoseconds) / 1e6, double(entry.queries.validationNanoseconds) / 1e6,
            double(entry.queries.queries) / double(entry.calls), unsigned(overflow));
    };
    for (unsigned index = 0; index < std::min(batch.used, 8u); ++index) print(*ranked[index], false);
    if (batch.overflow.calls) print(batch.overflow, true);
    batch.used = 0; batch.operations = 0;
    batch.sites = {}; batch.overflow = {};
    SetLastError(lastError);
}
class ImportMemoryQueryScope {
    bool enabled_;
    MemoryQueryCounters before_;
    uint32_t lr_ = 0, ordinal_ = 0;
    const char* library_ = nullptr;
    const char* name_ = nullptr;
public:
    ImportMemoryQueryScope(const PPCContext& ctx, const char* library, const char* name, uint32_t ordinal) :
        enabled_(memoryQueryTimingEnabled()), before_(enabled_ ? memoryQueryCounters() : MemoryQueryCounters{}) {
        // Generated import wrappers pass their static string literals. Keep
        // that identity without guest string reads or per-call string copies.
        if (enabled_) { lr_ = uint32_t(ctx.lr); ordinal_ = ordinal; library_ = library; name_ = name; }
    }
    ~ImportMemoryQueryScope() {
        if (enabled_) recordImportQuerySite(lr_, ordinal_, library_, name_, before_);
    }
};
void validate(uint32_t address, uint32_t bytes) {
    if (!memory || !memory->valid(address, bytes)) fail("Native import received inaccessible guest pointer");
}
std::shared_ptr<std::recursive_mutex> critical(uint32_t address) {
    std::lock_guard guard(stateMutex);
    auto found = criticalSections.find(address);
    if (found == criticalSections.end()) {
        validateInitialGuestCriticalSection(address);
        found = criticalSections.emplace(address, std::make_shared<std::recursive_mutex>()).first;
    }
    return found->second;
}
std::shared_ptr<std::mutex> spin(uint32_t address) {
    validate(address, 4);
    std::lock_guard guard(stateMutex);
    auto& lock = spinLocks[address];
    if (!lock) lock = std::make_shared<std::mutex>();
    return lock;
}
std::string guestString(uint32_t address) {
    std::string result;
    for (uint32_t index = 0; index < 65536; ++index) {
        if (uint64_t(address) + index > 0xffffffffull) fail("Guest string exceeds address space");
        validate(address + index, 1);
        char value = char(memory->base()[address + index]);
        if (!value) return result;
        result += value;
    }
    fail("Guest string exceeds supported bound");
}
void write16(uint32_t address, uint16_t value) {
    validate(address, 2);
    value = _byteswap_ushort(value);
    memcpy(memory->base() + address, &value, 2);
}
}

void DoritosNativeImport(PPCContext& ctx, uint8_t* base, const char* library, const char* name, uint32_t ordinal) {
    using namespace Doritos::Native;
    ImportMemoryQueryScope importQueryTiming(ctx, library, name, ordinal);
    currentContext = &ctx;
    { std::lock_guard guard(stateMutex);
      if (tracedImports.find(ImportTraceKey{library, name}) == tracedImports.end()) {
          tracedImports.insert(std::string(library) + "!" + name);
          fprintf(stderr, "[IMPORT] %s!%s ordinal=%u lr=0x%08X\n", library, name, ordinal, uint32_t(ctx.lr));
      } }
    const std::string_view function(name);
    auto result = [&](uint64_t value) { ctx.r3.u64 = value; };
    if (environmentImport(ctx, base, name)) return;
    if (synchronizationImport(ctx, base, name)) return;
    if (filesystemImport(ctx, base, name)) return;
    if (memoryServicesImport(ctx, base, name)) return;
    if (audioServicesImport(ctx, base, name)) return;
    if (xmaServicesImport(ctx, base, name)) return;
    if (userServicesImport(ctx, base, name)) return;
    if (inputServicesImport(ctx, base, name)) return;
    if (networkServicesImport(ctx, base, name)) return;
    if (function == "KeEnableFpuExceptions") {
        // Original CRT 825EB0C0 calls this void function with zero after
        // masking every IEEE exception. Apply that real native thread state
        // and refresh the translated FPSCR cache before later FP operations.
        if (ctx.r3.u32) fail("Enabled FPU exceptions require a verified native contract");
        if (DoritosNativeDisableFpuExceptions()) fail("Cannot mask native floating-point exceptions");
        ctx.fpscr.loadFromHost();
        return;
    }
    if (function == "XGetAVPack") {
        // Original CRT at 0x823E15D8 accepts 6 as the computer display path.
        result(6); return;
    }
    if (function == "RtlNtStatusToDosError") {
        using Convert = ULONG (WINAPI*)(LONG);
        static auto convert = reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
        if (!convert) fail("Native NT status conversion is unavailable");
        result(convert(ctx.r3.s32)); return;
    }
    if (function == "RtlInitializeCriticalSection" || function == "RtlInitializeCriticalSectionAndSpinCount") {
        uint32_t address = ctx.r3.u32;
        validate(address, 28);
        auto host = std::make_shared<std::recursive_mutex>();
        { std::lock_guard guard(stateMutex); criticalSections[address] = host; }
        memset(base + address, 0, 28);
        memory->write32(address, 0x01000400);
        memory->write32(address + 8, address + 8);
        memory->write32(address + 12, address + 8);
        memory->write32(address + 16, 0xffffffff);
        result(0); return;
    }
    if (function == "RtlEnterCriticalSection" || function == "RtlTryEnterCriticalSection") {
        const auto operation = function == "RtlTryEnterCriticalSection" ?
            CriticalSectionOperation::TryEnter : CriticalSectionOperation::Enter;
        CriticalMemoryQueryScope queryTiming(ctx, operation);
        CriticalSectionCallTiming timing(operation);
        uint32_t address = ctx.r3.u32;
        auto host = critical(address);
        bool acquired = true;
        if (function == "RtlTryEnterCriticalSection") acquired = host->try_lock();
        else host->lock();
        if (acquired) enterGuestCriticalSection(address, ctx.r13.u32);
        result(function == "RtlTryEnterCriticalSection" ? acquired : 0); return;
    }
    if (function == "RtlLeaveCriticalSection") {
        CriticalMemoryQueryScope queryTiming(ctx, CriticalSectionOperation::Leave);
        CriticalSectionCallTiming timing(CriticalSectionOperation::Leave);
        uint32_t address = ctx.r3.u32;
        auto host = critical(address);
        leaveGuestCriticalSection(address, ctx.r13.u32);
        host->unlock(); result(0); return;
    }
    if (function == "KfAcquireSpinLock" || function == "KeAcquireSpinLockAtRaisedIrql") {
        uint32_t address = ctx.r3.u32;
        spin(address)->lock(); memory->write32(address, ctx.r13.u32 ? ctx.r13.u32 : 1);
        uint32_t old = irql; irql = 2; result(old); return;
    }
    if (function == "KfReleaseSpinLock" || function == "KeReleaseSpinLockFromRaisedIrql") {
        uint32_t address = ctx.r3.u32;
        memory->write32(address, 0); spin(address)->unlock();
        if (function == "KfReleaseSpinLock") irql = ctx.r4.u32;
        return;
    }
    if (function == "KeRaiseIrqlToDpcLevel") { uint32_t old = irql; irql = 2; result(old); return; }
    if (function == "KeGetCurrentProcessType") { result(1); return; }
    if (function == "KeQueryPerformanceFrequency") { result(49875000); return; }
    if (function == "KeQueryPerformanceCounter") { result(PPCQueryTimebase()); return; }
    if (function == "KeQuerySystemTime") {
        FILETIME time{}; GetSystemTimeAsFileTime(&time);
        memory->write64(ctx.r3.u32, uint64_t(time.dwLowDateTime) | (uint64_t(time.dwHighDateTime) << 32)); return;
    }
    if (function == "KeTlsAlloc") {
        std::lock_guard guard(stateMutex);
        for (uint32_t index = 0; index < 64; ++index) if (!tlsAllocated[index]) { tlsAllocated[index] = true; result(index); return; }
        result(0xffffffff); return;
    }
    if (function == "KeTlsFree") {
        uint32_t index = ctx.r3.u32;
        std::lock_guard guard(stateMutex);
        if (index >= 64 || !tlsAllocated[index]) { result(0); return; }
        tlsAllocated[index] = false; memory->write32(memory->read32(ctx.r13.u32) + index * 4, 0); result(1); return;
    }
    if (function == "KeTlsGetValue" || function == "KeTlsSetValue") {
        uint32_t index = ctx.r3.u32;
        if (index >= 64) { result(0); return; }
        uint32_t address = memory->read32(ctx.r13.u32) + index * 4;
        if (function == "KeTlsGetValue") result(memory->read32(address));
        else { memory->write32(address, ctx.r4.u32); result(1); }
        return;
    }
    if (function == "RtlFillMemoryUlong") {
        uint32_t address = ctx.r3.u32, bytes = ctx.r4.u32, value = ctx.r5.u32;
        if (bytes & 3) fail("Unaligned RtlFillMemoryUlong length");
        if (bytes) validate(address, bytes);
        for (uint32_t offset = 0; offset < bytes; offset += 4) memory->write32(address + offset, value);
        return;
    }
    if (function == "RtlInitAnsiString") {
        uint32_t address = ctx.r3.u32, source = ctx.r4.u32;
        validate(address, 8);
        auto text = source ? guestString(source) : std::string();
        if (text.size() > 0xfffe) fail("Guest ANSI string too long");
        write16(address, uint16_t(text.size())); write16(address + 2, source ? uint16_t(text.size() + 1) : 0);
        memory->write32(address + 4, source); return;
    }
    if (function == "RtlImageXexHeaderField") {
        uint32_t header = ctx.r3.u32, key = ctx.r4.u32;
        if (header != 0x81000000) fail("Unsupported XEX header lookup source");
        uint32_t count = memory->read32(header + 20);
        for (uint32_t index = 0; index < count; ++index) {
            uint32_t record = header + 24 + index * 8;
            if (memory->read32(record) != key) continue;
            uint32_t value = memory->read32(record + 4);
            result((key & 0xff) <= 1 ? record + 4 : header + value); return;
        }
        result(0); return;
    }
    if (function == "XexCheckExecutablePrivilege") {
        uint32_t privilege = ctx.r3.u32, header = 0x81000000, flags = 0;
        uint32_t count = memory->read32(header + 20);
        for (uint32_t index = 0; index < count; ++index) {
            uint32_t record = header + 24 + index * 8;
            if (memory->read32(record) == 0x30000) { flags = memory->read32(record + 4); break; }
        }
        result(privilege < 32 && (flags & (1u << privilege)) ? 1 : 0); return;
    }
    if (function == "RtlLowerChar" || function == "RtlUpperChar") {
        uint8_t value = ctx.r3.u8;
        result(function == "RtlLowerChar" ? std::tolower(value) : std::toupper(value)); return;
    }
    if (function == "KeBugCheck" || function == "KeBugCheckEx") fail("Title invoked kernel bug check");
    fprintf(stderr, "[MISSING NATIVE IMPORT] %s!%s ordinal=%u\n", library, name, ordinal);
    fail("Import requires a verified native implementation");
}
