#include "runtime.h"
#include "kernel_objects.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <condition_variable>
#include <string>
#include <map>
#include <limits>
#include <mutex>
#include <string_view>
#include <vector>

namespace Doritos::Native {
namespace {
std::mutex objectMutex;
std::map<uint32_t, std::shared_ptr<KernelObject>> objects;
std::map<uint32_t, std::pair<std::shared_ptr<KernelObject>, uint32_t>> references;
std::vector<std::shared_ptr<KernelObject>> workers;
std::shared_ptr<KernelObject> mainThread;
std::map<ObjectKind, uint32_t> objectTypes;
std::map<uint32_t, std::shared_ptr<KernelObject>> embeddedEvents;
std::mutex namespaceMutex;
std::map<std::string, std::weak_ptr<KernelObject>> namedObjects;
struct ReadWriteLock {
    std::mutex mutex;
    std::condition_variable condition;
    uint32_t readers = 0, waitingReaders = 0, waitingWriters = 0;
    DWORD writer = 0;
    std::map<DWORD, uint32_t> readerOwners;
};
std::map<uint32_t, std::shared_ptr<ReadWriteLock>> readWriteLocks;
std::map<uint32_t, TitleTerminationNotification> terminationNotifications;
thread_local uint32_t criticalRegionDepth = 0;
thread_local HANDLE dispatcherCancellation = nullptr;
uint32_t nextObject = 0x400;
bool cpuSpan(uint32_t address, uint32_t bytes) {
    const uint64_t end = uint64_t(address) + bytes;
    return address >= 0x10000 && bytes && end <= 0x100000000ull &&
        !(address < 0x7fd00000u && end > 0x7fc00000u) &&
        !(address < 0x7ff00000u && end > 0x7fe00000u);
}
class CriticalSectionSpan {
    uint8_t* bytes_;
public:
    explicit CriticalSectionSpan(uint32_t address) {
        if (!memory || !cpuSpan(address, 28) || !memory->valid(address, 28))
            fail("Native import received inaccessible guest pointer");
        bytes_ = memory->base() + address;
    }
    uint32_t read(unsigned offset) const {
        uint32_t word; memcpy(&word, bytes_ + offset, sizeof(word));
        return _byteswap_ulong(word);
    }
    void write(unsigned offset, uint32_t word) const {
        word = _byteswap_ulong(word); memcpy(bytes_ + offset, &word, sizeof(word));
    }
};
uint32_t criticalSectionThread(uint32_t pcrAddress) {
    const uint64_t address = uint64_t(pcrAddress) + 0x100;
    if (address > UINT32_MAX || !cpuSpan(uint32_t(address), 4))
        fail("Native import read of inaccessible guest memory");
    return memory->read32(uint32_t(address));
}
struct CriticalTimingSample { uint64_t calls = 0, wall = 0, worst = 0; };
struct CriticalTimingBatch {
    std::array<CriticalTimingSample, 3> operations{};
    uint64_t began = 0, calls = 0;
    unsigned reports = 0;
};
thread_local CriticalTimingBatch criticalTiming;
bool criticalTimingEnabled() noexcept {
    static const bool enabled = [] {
        const DWORD lastError = GetLastError();
        wchar_t flag[8]{};
        const bool selected = GetEnvironmentVariableW(L"DORITOS_KERNEL_TIMING", flag, 8) == 1 && flag[0] == L'1';
        SetLastError(lastError);
        return selected;
    }();
    return enabled;
}
uint64_t criticalClock() noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
using NtWait = LONG (NTAPI*)(HANDLE, BOOLEAN, const LARGE_INTEGER*);
using NtWaitMany = LONG (NTAPI*)(ULONG, HANDLE*, ULONG, BOOLEAN, const LARGE_INTEGER*);
using NtDelay = LONG (NTAPI*)(BOOLEAN, const LARGE_INTEGER*);
using NtSet = LONG (NTAPI*)(HANDLE, LONG*);
template<class Function> Function ntdll(const char* name) {
    auto value = reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name));
    if (!value) fail("Required native Windows kernel contract unavailable");
    return value;
}
LONG nativeWait(std::vector<HANDLE> handles, ULONG type, BOOLEAN alertable, const LARGE_INTEGER* timeout) {
    static auto wait = ntdll<NtWaitMany>("NtWaitForMultipleObjects");
    if (!dispatcherCancellation) return wait(ULONG(handles.size()), handles.data(), type, alertable, timeout);
    if (std::find(handles.begin(), handles.end(), dispatcherCancellation) != handles.end())
        fail("Dispatcher cancellation event aliases a guest wait object");
    if (type == 1 && handles.size() < MAXIMUM_WAIT_OBJECTS) {
        handles.insert(handles.begin(), dispatcherCancellation);
        LONG status = wait(ULONG(handles.size()), handles.data(), 1, alertable, timeout);
        if (status == 0) throw DispatcherWaitCancelled{};
        if (status > 0 && status < LONG(handles.size())) return status - 1;
        if (status >= 0x81 && status < LONG(0x80 + handles.size())) return status - 1;
        return status;
    }
    // WaitAll must acquire/consume the actual guest set atomically. A short
    // native timeout preserves that contract while allowing host teardown to
    // interrupt an otherwise infinite wait. Relative budgets use monotonic
    // interrupt time; absolute deadlines retain the native system-time epoch.
    ULONGLONG began = 0;
    if (timeout && timeout->QuadPart < 0) QueryInterruptTimePrecise(&began);
    const uint64_t budget = timeout && timeout->QuadPart < 0 ? uint64_t(-(timeout->QuadPart + 1)) + 1 : 0;
    for (;;) {
        DWORD cancelled = WaitForSingleObject(dispatcherCancellation, 0);
        if (cancelled == WAIT_OBJECT_0) throw DispatcherWaitCancelled{};
        if (cancelled != WAIT_TIMEOUT) fail("Invalid dispatcher cancellation event");
        uint64_t remaining = 100000; // 10 ms in 100 ns units.
        bool expired = false;
        if (timeout) {
            if (timeout->QuadPart < 0) {
                ULONGLONG now = 0; QueryInterruptTimePrecise(&now);
                uint64_t elapsed = now - began;
                expired = elapsed >= budget;
                remaining = expired ? 0 : std::min(remaining, budget - elapsed);
            } else {
                FILETIME now{}; GetSystemTimePreciseAsFileTime(&now);
                uint64_t absolute = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
                expired = absolute >= uint64_t(timeout->QuadPart);
                remaining = expired ? 0 : std::min(remaining, uint64_t(timeout->QuadPart) - absolute);
            }
        }
        LARGE_INTEGER slice{}; slice.QuadPart = -int64_t(remaining);
        LONG status = wait(ULONG(handles.size()), handles.data(), type, alertable, &slice);
        if (status != 0x102 || expired) return status;
    }
}
void output(uint32_t address, uint32_t bytes) {
    if (!address || !memory || !memory->valid(address, bytes)) fail("Inaccessible kernel output pointer");
}
struct NamedAttributes { std::string name; bool openIf = false; };
NamedAttributes attributes(uint32_t address) {
    if (!address) return {};
    output(address, 12);
    uint32_t root = memory->read32(address), name = memory->read32(address + 4), flags = memory->read32(address + 8);
    // sub_823E49F8 writes Xbox OBJECT_ATTRIBUTES {root=-4,name,flags=0x80}.
    // Its name is an ANSI_STRING, not the Windows UNICODE_STRING ABI.
    if (root && root != 0xfffffffcu) fail("Unsupported kernel object namespace root");
    if (flags & ~0xc2u) fail("Unsupported kernel object namespace attributes");
    NamedAttributes value; value.openIf = (flags & 0x80) != 0;
    if (!name) return value;
    output(name, 8);
    uint32_t packed = memory->read32(name), length = packed >> 16, maximum = packed & 0xffff;
    uint32_t buffer = memory->read32(name + 4);
    if (length > maximum) fail("Invalid kernel object ANSI_STRING length");
    if (!length) return value;
    output(buffer, length);
    value.name.assign(reinterpret_cast<const char*>(memory->base() + buffer), length);
    if (value.name.find('\0') != std::string::npos) fail("Embedded NUL in kernel object name");
    if (flags & 0x40) for (char& byte : value.name) byte = char(std::tolower(uint8_t(byte)));
    value.name.insert(0, root ? "title:" : "absolute:");
    return value;
}
std::shared_ptr<KernelObject> named(const NamedAttributes& attr, ObjectKind kind, uint32_t& status) {
    if (attr.name.empty()) return nullptr;
    auto found = namedObjects.find(attr.name);
    if (found == namedObjects.end()) return nullptr;
    auto object = found->second.lock();
    if (!object) { namedObjects.erase(found); return nullptr; }
    status = object->kind != kind ? 0xc0000024 : !attr.openIf ? 0xc0000035 : 0x40000000;
    return object;
}
uint32_t storeAlias(const std::shared_ptr<KernelObject>& object) {
    std::lock_guard guard(objectMutex);
    if (nextObject >= 0x08000000) fail("Guest kernel handle space exhausted");
    uint32_t token = nextObject++; objects.emplace(token, object); return token;
}
void publish32(uint32_t address, uint32_t value) {
    output(address, 4);
    if (address & 3) fail("Unaligned guest dispatcher field");
    InterlockedExchange(reinterpret_cast<volatile LONG*>(memory->base() + address), LONG(_byteswap_ulong(value)));
}
void initializeEventHeader(uint32_t address, uint32_t type, bool state) {
    output(address, 16);
    memory->write32(address, (type << 24) | 0x400);
    memory->write32(address + 4, state);
    memory->write32(address + 8, address + 8); memory->write32(address + 12, address + 8);
}
void refreshEvent(const std::shared_ptr<KernelObject>& object) {
    if (!object || object->kind != ObjectKind::Event || !object->guestAddress) return;
    std::lock_guard guard(object->dispatcherMutex);
    struct EventInfo { LONG type, state; } info{};
    using Query = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    static auto query = ntdll<Query>("NtQueryEvent");
    if (query(object->handle, 0, &info, sizeof(info), nullptr) < 0) fail("Unable to query native dispatcher event state");
    publish32(object->guestAddress + 4, uint32_t(info.state));
}
std::shared_ptr<KernelObject> dispatcher(uint32_t address) {
    auto existing = lookupObjectAddress(address);
    if (existing && (existing->kind != ObjectKind::Event || existing->owner)) return existing;
    output(address, 16);
    // A guest signal snapshot taken before this lock can outlive a concurrent
    // KeSetEvent/KeResetEvent and undo its newer native state during reconciliation.
    std::unique_lock<std::recursive_mutex> eventGuard;
    if (existing) eventGuard = std::unique_lock(existing->dispatcherMutex);
    auto* bytes = memory->base() + address;
    const uint32_t state = memory->read32(address + 4);
    // Actual title factory sub_8261FB00 allocates 20 bytes, writes only byte 0
    // (event type), +4 (signal), self list +8/+12 and its own refcount +16.
    // Header bytes 1..3 are untouched padding; requiring the Windows size byte
    // rejects this valid Xbox event. The self list still rejects zeroed storage.
    if (bytes[0] > 1 || state > 1 ||
        memory->read32(address + 8) != address + 8 || memory->read32(address + 12) != address + 8)
        fail("Unsupported or uninitialized embedded dispatcher object");
    if (existing && existing->manualReset == (bytes[0] == 0)) {
        // The original thread wrapper sub_8261CE28 resets the guest signal
        // field directly between worker runs. Reconcile this when no wait is
        // consuming a signal; imports serialize their own state publication.
        if (!existing->activeWaiters.load()) {
            struct EventInfo { LONG type, state; } info{};
            using Query = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
            static auto query = ntdll<Query>("NtQueryEvent");
            if (query(existing->handle,0,&info,sizeof(info),nullptr) < 0) fail("Native event query failed");
            if (uint32_t(info.state) != state && !(state ? SetEvent(existing->handle) : ResetEvent(existing->handle)))
                fail("Native embedded event signal reconciliation failed");
        }
        return existing;
    }
    if (existing && existing->activeWaiters.load()) fail("Cannot reinitialize an embedded event while it is waiting");
    auto object = std::make_shared<KernelObject>();
    object->handle = CreateEventW(nullptr, bytes[0] == 0, state != 0, nullptr);
    if (!object->handle) fail("Native embedded event creation failed");
    object->kind = ObjectKind::Event; object->guestAddress = address; object->manualReset = bytes[0] == 0;
    std::lock_guard guard(objectMutex);
    if (existing) { embeddedEvents[address] = object; return object; }
    return embeddedEvents.emplace(address, object).first->second;
}
void beginDispatcherWait(const std::shared_ptr<KernelObject>& object) {
    // Reconciliation must see this reservation before native waits can consume
    // an auto-reset signal while its guest mirror is still signaled.
    std::lock_guard guard(object->dispatcherMutex);
    object->activeWaiters.fetch_add(1);
}
void finishDispatcherWait(const std::shared_ptr<KernelObject>& object) {
    std::lock_guard guard(object->dispatcherMutex);
    refreshEvent(object);
    object->activeWaiters.fetch_sub(1);
}
bool setAffinity(const std::shared_ptr<KernelObject>& object, uint32_t mask) {
    if (!mask || (mask & ~0x3fu)) return false;
    DWORD_PTR allowed = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) return false;
    std::vector<DWORD_PTR> processors;
    for (unsigned index = 0; index < sizeof(DWORD_PTR) * 8; ++index)
        if (allowed & (DWORD_PTR(1) << index)) processors.push_back(DWORD_PTR(1) << index);
    if (processors.empty()) return false;
    DWORD_PTR hostMask = 0;
    for (unsigned guest = 0; guest < 6; ++guest) if (mask & (1u << guest)) hostMask |= processors[guest % processors.size()];
    if (!SetThreadAffinityMask(object->handle, hostMask)) return false;
    object->affinity.store(mask);
    if (object->guestAddress) {
        unsigned processor = 0;
        while (!(mask & (1u << processor))) ++processor;
        InterlockedExchange8(reinterpret_cast<volatile CHAR*>(memory->base() + object->guestAddress - 0xd00 + 0x10c), CHAR(processor));
    }
    return true;
}
std::shared_ptr<ReadWriteLock> readWriteLock(uint32_t address) {
    output(address, 0x38);
    std::lock_guard guard(objectMutex);
    auto found = readWriteLocks.find(address);
    if (found == readWriteLocks.end()) fail("Uninitialized native read/write lock");
    return found->second;
}
void publishReadWriteLock(uint32_t address, const ReadWriteLock& state) {
    uint32_t entrants = state.readers + (state.writer != 0) + state.waitingReaders + state.waitingWriters;
    publish32(address, entrants - 1); publish32(address + 4, state.waitingWriters);
    publish32(address + 8, state.waitingReaders); publish32(address + 12, state.readers);
}
void callCompiled(PPCContext& ctx, uint32_t address) {
    if (address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE || (address & 3))
        fail("Native thread entry lies outside compiled code");
    auto entry = PPC_LOOKUP_FUNC(memory->base(), address);
    if (!entry) fail("Native thread entry has no compiled function");
    int status = runGuest(ctx, memory->base(), entry);
    if (status == 3) ExitProcess(3);
}
struct ThreadStart {
    PPCContext ctx{};
    uint32_t entry, startup, argument;
    std::shared_ptr<KernelObject> object;
};
DWORD WINAPI workerMain(void* raw) {
    std::unique_ptr<ThreadStart> start(static_cast<ThreadStart*>(raw));
    currentContext = &start->ctx;
    uint32_t thread = memory->read32(start->ctx.r13.u32 + 0x100);
    memory->write32(thread + 0x14c, GetCurrentThreadId());
    start->ctx.fpscr.loadFromHost();
    fprintf(stderr, "[THREAD] native worker id=%lu entry=0x%08X startup=0x%08X\n", GetCurrentThreadId(), start->entry, start->startup);
    uint32_t result = 0;
    try {
        start->ctx.r3.u64 = start->startup ? start->entry : start->argument;
        start->ctx.r4.u64 = start->argument;
        callCompiled(start->ctx, start->startup ? start->startup : start->entry);
        result = start->ctx.r3.u32;
    } catch (const GuestThreadExit& exit) { result = exit.code; }
    catch (const std::exception& error) {
        fprintf(stderr, "[THREAD STOP] %s\n", error.what());
        fflush(stderr);
        ExitProcess(1);
    }
    publish32(thread + 320, result);
    publish32(thread + 4, (memory->read32(thread + 4) & 0xffffff00u) | 1u);
    currentContext = nullptr;
    return result;
}
}
void validateInitialGuestCriticalSection(uint32_t address) {
    const CriticalSectionSpan object(address);
    if (object.read(0) != 0x01000400 || object.read(4) ||
        object.read(8) != address + 8 || object.read(12) != address + 8 ||
        object.read(16) != 0xffffffff || object.read(20) || object.read(24))
        fail("Uninitialized guest critical section");
}
void enterGuestCriticalSection(uint32_t address, uint32_t pcrAddress) {
    const CriticalSectionSpan object(address);
    // Preserve the original import's update order, unsigned wrap, recursion
    // count and owning guest-thread pointer. The native mutex is held here.
    object.write(16, object.read(16) + 1);
    object.write(20, object.read(20) + 1);
    object.write(24, criticalSectionThread(pcrAddress));
}
void leaveGuestCriticalSection(uint32_t address, uint32_t pcrAddress) {
    const CriticalSectionSpan object(address);
    const uint32_t depth = object.read(20);
    if (!depth || object.read(24) != criticalSectionThread(pcrAddress))
        fail("Guest critical-section ownership mismatch");
    object.write(20, depth - 1);
    object.write(16, object.read(16) - 1);
    if (depth == 1) object.write(24, 0);
}
CriticalSectionCallTiming::CriticalSectionCallTiming(CriticalSectionOperation operation) noexcept : operation_(operation) {
    if (!criticalTimingEnabled() || criticalTiming.reports >= 12 || unsigned(operation) >= 3) return;
    const DWORD lastError = GetLastError();
    started_ = criticalClock();
    if (!criticalTiming.calls) criticalTiming.began = started_;
    SetLastError(lastError);
}
CriticalSectionCallTiming::~CriticalSectionCallTiming() noexcept {
    if (!started_) return;
    const DWORD lastError = GetLastError();
    const uint64_t ended = criticalClock(), elapsed = ended - started_;
    auto& sample = criticalTiming.operations[unsigned(operation_)];
    ++sample.calls; sample.wall += elapsed; sample.worst = std::max(sample.worst, elapsed);
    if (++criticalTiming.calls != 10000) { SetLastError(lastError); return; }
    const auto& enter = criticalTiming.operations[0];
    const auto& attempt = criticalTiming.operations[1];
    const auto& leave = criticalTiming.operations[2];
    fprintf(stderr, "[CRITICAL SECTION WALL] thread=%lu batch=%u calls=%llu interval_ms=%.3f "
        "enter_calls=%llu enter_wall_ms=%.3f enter_worst_ms=%.3f "
        "try_calls=%llu try_wall_ms=%.3f try_worst_ms=%.3f "
        "leave_calls=%llu leave_wall_ms=%.3f leave_worst_ms=%.3f "
        "scope=cpu_wall_calls includes_native_lock_waits=true cpu_execution_measured=false\n",
        GetCurrentThreadId(), ++criticalTiming.reports, criticalTiming.calls, double(ended - criticalTiming.began) / 1e6,
        enter.calls, double(enter.wall) / 1e6, double(enter.worst) / 1e6,
        attempt.calls, double(attempt.wall) / 1e6, double(attempt.worst) / 1e6,
        leave.calls, double(leave.wall) / 1e6, double(leave.worst) / 1e6);
    criticalTiming.calls = 0; criticalTiming.operations = {};
    SetLastError(lastError);
}
KernelObject::~KernelObject() {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    if (owner && owner == memory) for (uint32_t address : allocations) owner->release(address);
}
HANDLE setDispatcherCancellation(HANDLE event) {
    HANDLE previous = dispatcherCancellation;
    dispatcherCancellation = event;
    return previous;
}
uint32_t registerObject(HANDLE handle, ObjectKind kind, uint32_t guestAddress) {
    if (!handle || handle == INVALID_HANDLE_VALUE) fail("Invalid native kernel handle registration");
    auto object = std::make_shared<KernelObject>();
    object->handle = handle; object->kind = kind; object->guestAddress = guestAddress;
    std::lock_guard guard(objectMutex);
    if (nextObject >= 0x08000000) fail("Guest kernel handle space exhausted");
    uint32_t id = nextObject++;
    objects.emplace(id, object);
    return id;
}
std::shared_ptr<KernelObject> lookupObject(uint32_t handle) {
    if (handle == 0xfffffffeu && currentContext)
        return lookupObjectAddress(memory->read32(currentContext->r13.u32 + 0x100));
    std::lock_guard guard(objectMutex);
    auto found = objects.find(handle);
    return found == objects.end() ? nullptr : found->second;
}
std::shared_ptr<KernelObject> lookupObjectAddress(uint32_t address) {
    std::lock_guard guard(objectMutex);
    auto retained = references.find(address);
    if (retained != references.end()) return retained->second.first;
    if (mainThread && mainThread->guestAddress == address) return mainThread;
    auto embedded = embeddedEvents.find(address);
    if (embedded != embeddedEvents.end()) return embedded->second;
    for (const auto& [id, object] : objects) if (object->guestAddress == address && address) return object;
    for (const auto& worker : workers) if (worker->guestAddress == address && address) return worker;
    return nullptr;
}
std::shared_ptr<KernelObject> lookupThreadId(DWORD id) {
    std::lock_guard guard(objectMutex);
    if (mainThread && mainThread->threadId == id) return mainThread;
    for (const auto& worker : workers) if (worker->threadId == id) return worker;
    for (const auto& [token, object] : objects) if (object->kind == ObjectKind::Thread && object->threadId == id) return object;
    for (const auto& [address, reference] : references) if (reference.first->kind == ObjectKind::Thread && reference.first->threadId == id) return reference.first;
    return nullptr;
}
bool closeObject(uint32_t handle) {
    std::shared_ptr<KernelObject> removed;
    {
        std::lock_guard guard(objectMutex);
        auto found = objects.find(handle);
        if (found == objects.end()) return false;
        removed = std::move(found->second); objects.erase(found);
    }
    return true;
}
uint32_t duplicateObject(uint32_t handle) {
    auto object = lookupObject(handle);
    if (!object) return 0;
    std::lock_guard guard(objectMutex);
    if (nextObject >= 0x08000000) fail("Guest kernel handle space exhausted");
    uint32_t id = nextObject++;
    objects.emplace(id, object);
    return id;
}
void retainObject(const std::shared_ptr<KernelObject>& object) {
    if (!object || !object->guestAddress) fail("Kernel object has no guest representation");
    std::lock_guard guard(objectMutex);
    auto& reference = references[object->guestAddress];
    if (reference.second == std::numeric_limits<uint32_t>::max()) fail("Guest kernel reference count overflow");
    reference.first = object; ++reference.second;
}
bool releaseObject(uint32_t address) {
    std::shared_ptr<KernelObject> removed;
    { std::lock_guard guard(objectMutex);
      auto found = references.find(address);
      if (found == references.end()) return false;
      if (!--found->second.second) { removed = std::move(found->second.first); references.erase(found); } }
    return true;
}
bool liveGuestThreads() {
    std::vector<std::shared_ptr<KernelObject>> snapshot, completed;
    { std::lock_guard guard(objectMutex); snapshot = workers; }
    bool live = false;
    for (const auto& worker : snapshot) {
        DWORD state = WaitForSingleObject(worker->handle, 0);
        if (state == WAIT_TIMEOUT) live = true;
        if (state == WAIT_FAILED) fail("Native worker lifetime query failed");
        if (state == WAIT_OBJECT_0) completed.push_back(worker);
    }
    // The registry retains active/suspended workers independently of guest
    // handles. Once complete, normal handles and object references own lifetime.
    { std::lock_guard guard(objectMutex);
      std::erase_if(workers,[&](const auto& value) { return std::find(completed.begin(),completed.end(),value) != completed.end(); }); }
    return live;
}
void configureObjectType(ObjectKind kind, uint32_t address) {
    std::lock_guard guard(objectMutex); objectTypes[kind] = address;
}
void registerMainThread(PPCContext& ctx) {
    HANDLE handle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
        fail("Cannot register native primary thread");
    auto object = std::make_shared<KernelObject>();
    object->handle = handle; object->kind = ObjectKind::Thread;
    object->guestAddress = memory->read32(ctx.r13.u32 + 0x100); object->threadId = GetCurrentThreadId();
    std::lock_guard guard(objectMutex); mainThread = std::move(object);
}
std::vector<TitleTerminationNotification> titleTerminationNotifications() {
    std::lock_guard guard(objectMutex);
    std::vector<TitleTerminationNotification> snapshot;
    for (const auto& [registration, value] : terminationNotifications) snapshot.push_back(value);
    return snapshot;
}
void shutdownKernelObjects() {
    if (liveGuestThreads()) {
        fprintf(stderr, "[NATIVE STOP] Guest workers still own mapped memory; ending process before teardown.\n");
        fflush(stderr); ExitProcess(1);
    }
    std::map<uint32_t, std::shared_ptr<KernelObject>> removedObjects, removedEmbedded;
    std::map<uint32_t, std::pair<std::shared_ptr<KernelObject>, uint32_t>> removedReferences;
    std::vector<std::shared_ptr<KernelObject>> removedWorkers;
    std::shared_ptr<KernelObject> removedMain;
    { std::scoped_lock guard(namespaceMutex, objectMutex);
      removedObjects.swap(objects); removedEmbedded.swap(embeddedEvents); removedReferences.swap(references);
      removedWorkers.swap(workers); removedMain.swap(mainThread); objectTypes.clear(); namedObjects.clear(); }
    { std::lock_guard guard(objectMutex); readWriteLocks.clear(); terminationNotifications.clear(); }
}
bool synchronizationImport(PPCContext& ctx, uint8_t* base, const char* name) {
    const std::string_view function(name);
    auto result = [&](uint32_t value) { ctx.r3.u64 = value; return true; };
    if (function == "NtClose") return result(closeObject(ctx.r3.u32) ? 0 : 0xc0000008);
    if (function == "KeEnterCriticalRegion") {
        if (criticalRegionDepth == std::numeric_limits<uint32_t>::max()) fail("Critical region depth overflow");
        ++criticalRegionDepth; return true;
    }
    if (function == "KeLeaveCriticalRegion") {
        if (!criticalRegionDepth) fail("Critical region leave without matching enter");
        if (!--criticalRegionDepth) SleepEx(0, TRUE);
        return true;
    }
    if (function == "ExRegisterTitleTerminateNotification") {
        uint32_t registration = ctx.r3.u32;
        output(registration, 16);
        uint32_t routine = memory->read32(registration), priority = memory->read32(registration + 4);
        if (ctx.r4.u32) {
            if (routine < PPC_CODE_BASE || routine >= PPC_CODE_BASE + PPC_CODE_SIZE || (routine & 3) || !PPC_LOOKUP_FUNC(base, routine))
                fail("Title terminate registration has no compiled callback");
            std::lock_guard guard(objectMutex);
            terminationNotifications[registration] = {registration, routine, priority};
        } else {
            std::lock_guard guard(objectMutex); terminationNotifications.erase(registration);
        }
        return true;
    }
    if (function == "ExInitializeReadWriteLock") {
        uint32_t address = ctx.r3.u32; output(address, 0x38);
        auto state = std::make_shared<ReadWriteLock>();
        { std::lock_guard guard(objectMutex);
          if (auto old = readWriteLocks.find(address); old != readWriteLocks.end()) {
              std::lock_guard previous(old->second->mutex);
              if (old->second->writer || old->second->readers || old->second->waitingReaders || old->second->waitingWriters)
                  fail("Cannot reinitialize an occupied native read/write lock");
          }
          readWriteLocks[address] = state; }
        memory->write32(address, 0xffffffff); memory->write32(address+4,0);
        memory->write32(address+8,0); memory->write32(address+12,0);
        initializeEventHeader(address+16,1,false);
        memory->write32(address+32,0x05000500); memory->write32(address+36,0);
        memory->write32(address+40,address+40); memory->write32(address+44,address+40);
        memory->write32(address+48,0x7fffffff); memory->write32(address+52,0);
        return true;
    }
    if (function == "ExAcquireReadWriteLockExclusive" || function == "ExAcquireReadWriteLockShared") {
        uint32_t address = ctx.r3.u32; auto state = readWriteLock(address); DWORD owner = GetCurrentThreadId();
        std::unique_lock lock(state->mutex);
        if (state->writer == owner || state->readerOwners.contains(owner)) fail("Recursive native read/write lock acquisition");
        if (function == "ExAcquireReadWriteLockExclusive") {
            ++state->waitingWriters; publishReadWriteLock(address,*state);
            state->condition.wait(lock,[&] { return !state->writer && !state->readers; });
            --state->waitingWriters; state->writer = owner;
        } else {
            ++state->waitingReaders; publishReadWriteLock(address,*state);
            state->condition.wait(lock,[&] { return !state->writer && !state->waitingWriters; });
            --state->waitingReaders; ++state->readers; ++state->readerOwners[owner];
        }
        publishReadWriteLock(address,*state); return true;
    }
    if (function == "ExReleaseReadWriteLock") {
        uint32_t address = ctx.r3.u32; auto state = readWriteLock(address); DWORD owner = GetCurrentThreadId();
        std::lock_guard lock(state->mutex);
        if (state->writer == owner) state->writer = 0;
        else {
            auto found = state->readerOwners.find(owner);
            if (found == state->readerOwners.end()) fail("Read/write lock release without ownership");
            if (!--found->second) state->readerOwners.erase(found); --state->readers;
        }
        publishReadWriteLock(address,*state); state->condition.notify_all(); return true;
    }
    if (function == "NtCreateEvent") {
        output(ctx.r3.u32, 4);
        if (ctx.r5.u32 > 1) return result(0xc000000d);
        auto attr = attributes(ctx.r4.u32);
        std::lock_guard namespaceGuard(namespaceMutex);
        uint32_t status = 0;
        if (auto existing = named(attr, ObjectKind::Event, status)) {
            if (status == 0x40000000) memory->write32(ctx.r3.u32, storeAlias(existing));
            return result(status);
        }
        auto handle = CreateEventW(nullptr, ctx.r5.u32 == 0, ctx.r6.u32 != 0, nullptr);
        if (!handle) return result(0xc0000017);
        uint32_t token = registerObject(handle, ObjectKind::Event);
        auto object = lookupObject(token);
        object->owner = memory; object->manualReset = ctx.r5.u32 == 0;
        object->guestAddress = memory->allocate(16); object->allocations.push_back(object->guestAddress);
        initializeEventHeader(object->guestAddress, ctx.r5.u32, ctx.r6.u32 != 0);
        if (!attr.name.empty()) namedObjects[attr.name] = object;
        memory->write32(ctx.r3.u32, token);
        return result(0);
    }
    if (function == "NtSetEvent" || function == "NtClearEvent") {
        auto object = lookupObject(ctx.r3.u32);
        if (!object || object->kind != ObjectKind::Event) return result(0xc0000008);
        std::lock_guard eventGuard(object->dispatcherMutex);
        if (function == "NtClearEvent") {
            if (!ResetEvent(object->handle)) return result(0xc0000001);
            refreshEvent(object); return result(0);
        }
        if (ctx.r4.u32) output(ctx.r4.u32, 4);
        static auto set = ntdll<NtSet>("NtSetEvent");
        LONG previous = 0, status = set(object->handle, &previous);
        if (status >= 0) {
            refreshEvent(object);
            if (ctx.r4.u32) memory->write32(ctx.r4.u32, uint32_t(previous));
        }
        return result(uint32_t(status));
    }
    if (function == "KeSetEvent" || function == "KeResetEvent") {
        auto object = dispatcher(ctx.r3.u32);
        if (object->kind != ObjectKind::Event) fail("Guest event operation targeted a different dispatcher object");
        std::lock_guard eventGuard(object->dispatcherMutex);
        if (function == "KeSetEvent" && ctx.r5.u32) fail("Atomic set-event-and-wait scheduling is not implemented");
        static auto set = ntdll<NtSet>("NtSetEvent");
        static auto reset = ntdll<NtSet>("NtResetEvent");
        LONG previous = 0;
        LONG status = (function == "KeSetEvent" ? set : reset)(object->handle, &previous);
        if (status < 0) fail("Native dispatcher event operation failed");
        refreshEvent(object); return result(uint32_t(previous));
    }
    if (function == "NtCreateSemaphore") {
        output(ctx.r3.u32, 4);
        if (ctx.r5.s32 < 0 || ctx.r6.s32 <= 0 || ctx.r5.s32 > ctx.r6.s32) return result(0xc000000d);
        auto attr = attributes(ctx.r4.u32);
        std::lock_guard namespaceGuard(namespaceMutex);
        uint32_t status = 0;
        if (auto existing = named(attr, ObjectKind::Semaphore, status)) {
            if (status == 0x40000000) memory->write32(ctx.r3.u32, storeAlias(existing));
            return result(status);
        }
        auto handle = CreateSemaphoreW(nullptr, ctx.r5.s32, ctx.r6.s32, nullptr);
        if (!handle) return result(0xc0000017);
        uint32_t token = registerObject(handle, ObjectKind::Semaphore);
        if (!attr.name.empty()) namedObjects[attr.name] = lookupObject(token);
        memory->write32(ctx.r3.u32, token);
        return result(0);
    }
    if (function == "NtReleaseSemaphore") {
        auto object = lookupObject(ctx.r3.u32);
        if (!object || object->kind != ObjectKind::Semaphore) return result(0xc0000008);
        if (ctx.r5.u32) output(ctx.r5.u32, 4);
        if (ctx.r4.s32 <= 0) return result(0xc000000d);
        LONG previous = 0;
        if (!ReleaseSemaphore(object->handle, ctx.r4.s32, &previous)) return result(0xc0000047);
        if (ctx.r5.u32) memory->write32(ctx.r5.u32, uint32_t(previous));
        return result(0);
    }
    if (function == "NtWaitForSingleObjectEx" || function == "KeWaitForSingleObject") {
        bool direct = function == "KeWaitForSingleObject";
        auto object = direct ? dispatcher(ctx.r3.u32) : lookupObject(ctx.r3.u32);
        if (!object) return result(0xc0000008);
        LARGE_INTEGER timeout{}, *pointer = nullptr;
        uint32_t timeoutAddress = direct ? ctx.r7.u32 : ctx.r6.u32;
        if (timeoutAddress) { timeout.QuadPart = int64_t(memory->read64(timeoutAddress)); pointer = &timeout; }
        beginDispatcherWait(object);
        LONG status;
        try { status = nativeWait({object->handle}, 1, !criticalRegionDepth && (direct ? ctx.r6.u32 : ctx.r5.u32) != 0, pointer); }
        catch (...) { finishDispatcherWait(object); throw; }
        finishDispatcherWait(object); return result(uint32_t(status));
    }
    if (function == "NtWaitForMultipleObjectsEx" || function == "KeWaitForMultipleObjects") {
        bool direct = function == "KeWaitForMultipleObjects";
        uint32_t count = ctx.r3.u32;
        if (!count || count > MAXIMUM_WAIT_OBJECTS || ctx.r5.u32 > 1) return result(0xc000000d);
        output(ctx.r4.u32, count * 4);
        std::vector<std::shared_ptr<KernelObject>> retained; std::vector<HANDLE> handles;
        for (uint32_t index = 0; index < count; ++index) {
            uint32_t target = memory->read32(ctx.r4.u32 + index * 4);
            auto object = direct ? dispatcher(target) : lookupObject(target);
            if (!object) return result(0xc0000008);
            if (std::find(handles.begin(), handles.end(), object->handle) != handles.end()) return result(0xc000000d);
            handles.push_back(object->handle); retained.push_back(std::move(object));
        }
        uint32_t timeoutAddress = direct ? ctx.r9.u32 : ctx.r8.u32;
        LARGE_INTEGER timeout{}, *pointer = nullptr;
        if (timeoutAddress) { timeout.QuadPart = int64_t(memory->read64(timeoutAddress)); pointer = &timeout; }
        for (const auto& object : retained) beginDispatcherWait(object);
        LONG status;
        try { status = nativeWait(handles, ctx.r5.u32, !criticalRegionDepth && (direct ? ctx.r8.u32 : ctx.r7.u32) != 0, pointer); }
        catch (...) { for (const auto& object : retained) finishDispatcherWait(object); throw; }
        for (const auto& object : retained) finishDispatcherWait(object);
        return result(uint32_t(status));
    }
    if (function == "KeDelayExecutionThread") {
        if (!ctx.r5.u32) return result(0xc000000d);
        LARGE_INTEGER timeout; timeout.QuadPart = int64_t(memory->read64(ctx.r5.u32));
        static auto delay = ntdll<NtDelay>("NtDelayExecution");
        return result(uint32_t(delay(!criticalRegionDepth && ctx.r4.u32 != 0, &timeout)));
    }
    if (function == "RtlNtStatusToDosError") {
        using Convert = ULONG (NTAPI*)(LONG);
        static auto convert = ntdll<Convert>("RtlNtStatusToDosError");
        return result(convert(ctx.r3.s32));
    }
    if (function == "NtDuplicateObject") {
        if (ctx.r5.u32 & ~1u) fail("Unsupported native handle duplication mode");
        uint32_t original = ctx.r3.u32;
        auto object = lookupObject(original);
        if (!object) return result(0xc0000008);
        if (ctx.r4.u32) { output(ctx.r4.u32, 4); memory->write32(ctx.r4.u32, storeAlias(object)); }
        if (ctx.r5.u32 & 1) closeObject(original);
        return result(0);
    }
    if (function == "ExCreateThread") {
        uint32_t out = ctx.r3.u32, stack = ctx.r4.u32, idOut = ctx.r5.u32;
        uint32_t startup = ctx.r6.u32, entry = ctx.r7.u32, argument = ctx.r8.u32, flags = ctx.r9.u32;
        output(out, 4); if (idOut) output(idOut, 4);
        if (!entry || stack > 0x1000000) return result(0xc000000d);
        if (flags & ~0x3f000001u) fail("Unsupported native thread creation flags");
        if (entry < PPC_CODE_BASE || entry >= PPC_CODE_BASE + PPC_CODE_SIZE || (entry & 3) || !PPC_LOOKUP_FUNC(base, entry))
            return result(0xc000000d);
        if (startup && (startup < PPC_CODE_BASE || startup >= PPC_CODE_BASE + PPC_CODE_SIZE || (startup & 3) || !PPC_LOOKUP_FUNC(base, startup)))
            return result(0xc000000d);
        auto start = std::make_unique<ThreadStart>();
        start->entry = entry; start->startup = startup; start->argument = argument;
        memory->initializeThread(start->ctx, stack);
        uint32_t thread = memory->read32(start->ctx.r13.u32 + 0x100);
        uint32_t affinity = (flags >> 24) & 0x3f, processor = 0;
        if (affinity) while (!(affinity & (1u << processor))) ++processor;
        base[start->ctx.r13.u32 + 0x10c] = uint8_t(processor);
        DWORD id = 0;
        auto handle = CreateThread(nullptr, 64 * 1024 * 1024, workerMain, start.get(),
            CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &id);
        if (!handle) {
            memory->release(memory->read32(start->ctx.r13.u32 + 0x74));
            memory->release(memory->read32(start->ctx.r13.u32)); memory->release(start->ctx.r13.u32);
            return result(0xc0000017);
        }
        uint32_t token = registerObject(handle, ObjectKind::Thread, thread);
        start->object = lookupObject(token);
        start->object->threadId = id; start->object->affinity = affinity ? affinity : 0x3f;
        start->object->worker = true; start->object->owner = memory;
        start->object->allocations = {memory->read32(start->ctx.r13.u32 + 0x74), memory->read32(start->ctx.r13.u32), start->ctx.r13.u32};
        { std::lock_guard guard(objectMutex); workers.push_back(start->object); }
        if (!setAffinity(start->object, affinity ? affinity : 0x3f)) {
            fprintf(stderr, "[NATIVE STOP] Native worker affinity assignment failed\n"); fflush(stderr); ExitProcess(1);
        }
        memory->write32(thread + 0x14c, id);
        memory->write32(out, token); if (idOut) memory->write32(idOut, id);
        start.release();
        if (!(flags & 1) && ResumeThread(handle) == DWORD(-1)) fail("Native worker could not resume");
        return result(0);
    }
    if (function == "ExTerminateThread") throw GuestThreadExit{ctx.r3.u32};
    if (function == "NtResumeThread" || function == "NtSuspendThread") {
        auto object = lookupObject(ctx.r3.u32);
        if (!object || object->kind != ObjectKind::Thread) return result(0xc0000008);
        if (ctx.r4.u32) output(ctx.r4.u32, 4);
        DWORD previous = function == "NtResumeThread" ? ResumeThread(object->handle) : SuspendThread(object->handle);
        if (previous == DWORD(-1)) return result(0xc0000001);
        if (ctx.r4.u32) memory->write32(ctx.r4.u32, previous);
        return result(0);
    }
    if (function == "ObReferenceObjectByHandle") {
        auto object = lookupObject(ctx.r3.u32);
        if (!object || !object->guestAddress) return result(0xc0000008);
        if (ctx.r4.u32) {
            std::lock_guard guard(objectMutex);
            auto expected = objectTypes.find(object->kind);
            if (expected == objectTypes.end() || expected->second != ctx.r4.u32) return result(0xc0000024);
        }
        output(ctx.r5.u32, 4); retainObject(object); memory->write32(ctx.r5.u32, object->guestAddress);
        return result(0);
    }
    if (function == "ObDereferenceObject") {
        if (!releaseObject(ctx.r3.u32)) fail("Unowned native object dereference");
        return true;
    }
    if (function == "ObReferenceObject") {
        auto object = lookupObjectAddress(ctx.r3.u32);
        if (!object) fail("Unknown native object reference");
        retainObject(object); return true;
    }
    if (function == "ObLookupThreadByThreadId") {
        output(ctx.r4.u32, 4);
        auto object = lookupThreadId(ctx.r3.u32);
        if (!object) return result(0xc0000225);
        retainObject(object); memory->write32(ctx.r4.u32, object->guestAddress); return result(0);
    }
    if (function == "KeSetBasePriorityThread" || function == "KeQueryBasePriorityThread") {
        auto object = lookupObjectAddress(ctx.r3.u32);
        if (!object || object->kind != ObjectKind::Thread) fail("Unknown native thread priority object");
        int32_t previous = object->priority.load();
        if (function == "KeSetBasePriorityThread") {
            int32_t priority = ctx.r4.s32;
            if (!SetThreadPriority(object->handle, std::clamp(priority, -2, 2))) fail("Native thread priority change failed");
            object->priority.store(priority);
        }
        ctx.r3.s64 = previous; return true;
    }
    if (function == "KeSetAffinityThread") {
        auto object = lookupObjectAddress(ctx.r3.u32);
        if (!object || object->kind != ObjectKind::Thread || !ctx.r4.u32 || (ctx.r4.u32 & ~0x3fu)) return result(0xc000000d);
        if (ctx.r5.u32) output(ctx.r5.u32, 4);
        uint32_t previous = object->affinity.load();
        if (!setAffinity(object, ctx.r4.u32)) return result(0xc0000001);
        if (ctx.r5.u32) memory->write32(ctx.r5.u32, previous); return result(0);
    }
    if (function == "NtCreateTimer") {
        output(ctx.r3.u32, 4);
        if (ctx.r5.u32 > 1) return result(0xc000000d);
        auto attr = attributes(ctx.r4.u32); std::lock_guard namespaceGuard(namespaceMutex);
        uint32_t status = 0;
        if (auto existing = named(attr, ObjectKind::Timer, status)) {
            if (status == 0x40000000) memory->write32(ctx.r3.u32, storeAlias(existing));
            return result(status);
        }
        HANDLE handle = CreateWaitableTimerW(nullptr, ctx.r5.u32 == 0, nullptr);
        if (!handle) return result(0xc0000017);
        uint32_t token = registerObject(handle, ObjectKind::Timer);
        if (!attr.name.empty()) namedObjects[attr.name] = lookupObject(token);
        memory->write32(ctx.r3.u32, token); return result(0);
    }
    if (function == "NtSetTimerEx") {
        auto object = lookupObject(ctx.r3.u32);
        if (!object || object->kind != ObjectKind::Timer) return result(0xc0000008);
        // sub_82895628 supplies routine in r5, constant one in r6,
        // context in r7, resume in r8, period in r9 and zero in r10.
        if (ctx.r6.u32 != 1 || ctx.r10.u32 || ctx.r9.s32 < 0) return result(0xc000000d);
        if (ctx.r5.u32) fail("Guest timer APC callbacks require a native callback bridge");
        LARGE_INTEGER due{}; due.QuadPart = int64_t(memory->read64(ctx.r4.u32));
        return result(SetWaitableTimer(object->handle,&due,ctx.r9.s32,nullptr,nullptr,ctx.r8.u32 != 0) ? 0 : 0xc0000001);
    }
    if (function == "NtCancelTimer") {
        auto object = lookupObject(ctx.r3.u32);
        if (!object || object->kind != ObjectKind::Timer) return result(0xc0000008);
        if (ctx.r4.u32) output(ctx.r4.u32, 4);
        using Cancel = LONG (NTAPI*)(HANDLE, BOOLEAN*);
        static auto cancel = ntdll<Cancel>("NtCancelTimer");
        BOOLEAN previous = FALSE; LONG status = cancel(object->handle,&previous);
        if (status >= 0 && ctx.r4.u32) memory->write32(ctx.r4.u32,previous);
        return result(uint32_t(status));
    }
    return false;
}
}
