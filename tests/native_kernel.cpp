#include "runtime/native/kernel_objects.h"
#include "runtime/native/runtime.h"
#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <thread>
#include <vector>
#include <winternl.h>

PPCFuncMapping PPCFuncMappings[]{{0, nullptr}};
using namespace Doritos::Native;
namespace {
std::atomic<uint32_t> workerPCR{0}, workerArgument{0};
std::atomic<uint32_t> apcCount{0};
void CALLBACK countApc(ULONG_PTR) { apcCount.fetch_add(1); }
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// Observe the contender actually blocked in the only contended lock in this
// fixture, rather than relying on a sleep to assume it reached the import.
bool waitForBlockedThread(DWORD id) {
    using Query = LONG (NTAPI*)(SYSTEM_INFORMATION_CLASS, void*, ULONG, ULONG*);
    auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) return false;
    std::vector<uint8_t> buffer(1024 * 1024);
    const uint64_t deadline = GetTickCount64() + 10000;
    do {
        ULONG required = 0;
        LONG status = query(SystemProcessInformation, buffer.data(), ULONG(buffer.size()), &required);
        if (uint32_t(status) == 0xc0000004) { buffer.resize(required + 65536); continue; }
        if (status < 0) return false;
        auto* process = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(buffer.data());
        for (;;) {
            if (ULONG_PTR(process->UniqueProcessId) == GetCurrentProcessId()) {
                auto* threads = reinterpret_cast<SYSTEM_THREAD_INFORMATION*>(process + 1);
                for (ULONG index = 0; index < process->NumberOfThreads; ++index)
                    if (ULONG_PTR(threads[index].ClientId.UniqueThread) == id && threads[index].ThreadState == 5)
                        return true; // Waiting, with no other blocking operation in the fixture.
                break;
            }
            if (!process->NextEntryOffset) break;
            process = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(reinterpret_cast<uint8_t*>(process) + process->NextEntryOffset);
        }
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return false;
}
LONG hostEventState(HANDLE event) {
    struct EventInfo { LONG type, state; } info{};
    using Query = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryEvent"));
    if (!query || query(event, 0, &info, sizeof(info), nullptr) < 0) return -1;
    return info.state;
}
void testWorker(PPCContext& ctx, uint8_t* base) {
    workerPCR.store(ctx.r13.u32); workerArgument.store(ctx.r3.u32);
    require(memory->read32(memory->read32(ctx.r13.u32 + 0x100) + 0x14c) == GetCurrentThreadId(), "worker native thread ID");
    ctx.r3.u64 = 0x76543210;
    synchronizationImport(ctx, base, "ExTerminateThread");
}
void testStartup(PPCContext& ctx, uint8_t* base) {
    require(ctx.r3.u32 == PPC_CODE_BASE && ctx.r4.u32 == 0x10203040, "native startup arguments");
    ctx.r3.u64 = ctx.r4.u32; testWorker(ctx, base);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        auto image = loadOriginalXex(argv[1]);
        Memory guest(image); memory = &guest;
        PPCContext ctx{}; guest.initializeThread(ctx, 0x100000); currentContext = &ctx;
        registerMainThread(ctx);
        uint32_t data = guest.allocate(4096), eventType = data + 3800, threadType = data + 3820;
        configureObjectType(ObjectKind::Event, eventType); configureObjectType(ObjectKind::Thread, threadType);
        auto call = [&](const char* name, std::initializer_list<uint32_t> args) {
            ctx.r3.u64 = ctx.r4.u64 = ctx.r5.u64 = ctx.r6.u64 = ctx.r7.u64 = ctx.r8.u64 = ctx.r9.u64 = ctx.r10.u64 = 0;
            PPCRegister* regs[]{&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
            unsigned i = 0; for (auto value : args) regs[i++]->u64 = value;
            require(synchronizationImport(ctx, guest.base(), name), "unrecognized native kernel contract");
            return ctx.r3.u32;
        };
        const uint32_t critical = data + 2200;
        const std::array<uint32_t, 7> initialCritical{
            0x01000400, 0, critical + 8, critical + 8, 0xffffffff, 0, 0};
        guest.write32(critical - 4, 0x12345678); guest.write32(critical + 28, 0xfedcba98);
        for (unsigned i = 0; i < initialCritical.size(); ++i) guest.write32(critical + i * 4, initialCritical[i]);
        validateInitialGuestCriticalSection(critical);
        auto rejectsCritical = [&](auto action) {
            try { action(); } catch (const NativeFailure&) { return true; }
            return false;
        };
        for (unsigned i = 0; i < initialCritical.size(); ++i) {
            guest.write32(critical + i * 4, initialCritical[i] ^ 1);
            require(rejectsCritical([&] { validateInitialGuestCriticalSection(critical); }),
                "every original initialized critical-section field remains required");
            guest.write32(critical + i * 4, initialCritical[i]);
        }
        const uint32_t guestThread = guest.read32(ctx.r13.u32 + 0x100);
        const uint32_t otherPCR = guest.allocate(4096), otherGuestThread = data + 3000;
        guest.write32(otherPCR + 0x100, otherGuestThread);
        std::recursive_mutex nativeCritical;
        nativeCritical.lock(); enterGuestCriticalSection(critical, ctx.r13.u32);
        nativeCritical.lock(); enterGuestCriticalSection(critical, ctx.r13.u32);
        require(guest.read32(critical + 16) == 1 && guest.read32(critical + 20) == 2 &&
            guest.read32(critical + 24) == guestThread, "real recursive acquisition retains big-endian counts and guest owner");
        std::array<uint8_t, 28> ownedCritical{};
        memcpy(ownedCritical.data(), guest.base() + critical, ownedCritical.size());
        require(rejectsCritical([&] { leaveGuestCriticalSection(critical, otherPCR); }) &&
            !memcmp(ownedCritical.data(), guest.base() + critical, ownedCritical.size()),
            "wrong guest owner cannot mutate recursion state or authorize unlock");
        std::atomic<bool> concurrentEntered{false};
        std::jthread excluded([&] {
            if (nativeCritical.try_lock()) { concurrentEntered.store(true); nativeCritical.unlock(); }
        });
        excluded.join(); require(!concurrentEntered.load(), "recursive native ownership still excludes other threads");
        leaveGuestCriticalSection(critical, ctx.r13.u32); nativeCritical.unlock();
        require(guest.read32(critical + 16) == 0 && guest.read32(critical + 20) == 1 &&
            guest.read32(critical + 24) == guestThread, "nested leave retains the real owner");
        leaveGuestCriticalSection(critical, ctx.r13.u32); nativeCritical.unlock();
        std::jthread successor([&] {
            std::lock_guard lock(nativeCritical);
            enterGuestCriticalSection(critical, otherPCR);
            concurrentEntered.store(guest.read32(critical + 24) == otherGuestThread);
            leaveGuestCriticalSection(critical, otherPCR);
        });
        successor.join(); require(concurrentEntered.load(), "released native mutex allows a genuine successor owner");
        validateInitialGuestCriticalSection(critical);
        require(rejectsCritical([&] { leaveGuestCriticalSection(critical, 0); }),
            "unowned leave fails before attempting an invalid PCR read");
        require(guest.read32(critical - 4) == 0x12345678 && guest.read32(critical + 28) == 0xfedcba98,
            "critical-section span updates preserve both neighboring sentinels");
        const uint32_t reservedCritical = guest.reserve(4096);
        require(rejectsCritical([&] { enterGuestCriticalSection(reservedCritical, ctx.r13.u32); }) &&
            rejectsCritical([&] { leaveGuestCriticalSection(0xfffffff4, ctx.r13.u32); }),
            "uncommitted and wrapping critical-section spans reject before memory access");
        const uint32_t splitCritical = guest.allocate(8192), splitAddress = splitCritical + 4096 - 20;
        memset(guest.base() + splitAddress, 0x5a, 28);
        DWORD oldProtect = 0;
        require(VirtualProtect(guest.base() + splitCritical + 4096, 4096, PAGE_NOACCESS, &oldProtect) != 0,
            "isolated full-span protection contract setup");
        require(rejectsCritical([&] { enterGuestCriticalSection(splitAddress, ctx.r13.u32); }) &&
            guest.read32(splitAddress + 16) == 0x5a5a5a5a,
            "partial inaccessible object rejects without even its accessible state mutation");
        DWORD restoredProtect = 0;
        require(VirtualProtect(guest.base() + splitCritical + 4096, 4096, oldProtect, &restoredProtect) != 0,
            "isolated protection contract restore");
        for (uint32_t mmio : {0x7fc00000u, 0x7fe00000u}) {
            guest.commit(mmio, 4096);
            memset(guest.base() + mmio, 0x7b, 28);
            require(guest.valid(mmio, 28) && rejectsCritical([&] { enterGuestCriticalSection(mmio, ctx.r13.u32); }) &&
                rejectsCritical([&] { validateInitialGuestCriticalSection(mmio); }) && guest.read32(mmio + 16) == 0x7b7b7b7b,
                "even committed guest MMIO cannot become a CPU critical-section object");
        }
        guest.release(otherPCR); guest.release(reservedCritical); guest.release(splitCritical);
        SetLastError(0x2a5f);
        {
            CriticalSectionCallTiming timing(CriticalSectionOperation::Enter);
            require(GetLastError() == 0x2a5f, "optional timing configuration preserves native thread last-error state");
            SetLastError(0x3b60);
        }
        require(GetLastError() == 0x3b60, "optional critical-section timing preserves the operation's native last error");
        guest.write64(data + 80, 0);
        require(call("NtCreateEvent", {data,0,1,0}) == 0, "auto-reset event create");
        uint32_t event = guest.read32(data);
        require(call("NtWaitForSingleObjectEx", {event,1,0,data+80}) == 0x102, "unsignaled zero-time wait");
        require(call("NtSetEvent", {event,data+4}) == 0 && guest.read32(data+4) == 0, "set previous state");
        require(call("NtWaitForSingleObjectEx", {event,1,0,data+80}) == 0, "signaled auto-reset event");
        require(call("NtWaitForSingleObjectEx", {event,1,0,data+80}) == 0x102, "event signal consumed");
        require(call("ObReferenceObjectByHandle", {event,threadType,data+8}) == 0xc0000024, "object reference type mismatch");
        require(call("ObReferenceObjectByHandle", {event,eventType,data+8}) == 0, "typed event reference");
        uint32_t eventAddress = guest.read32(data+8);
        require(call("NtDuplicateObject", {event,data+12,1}) == 0, "duplicate with close source");
        uint32_t alias = guest.read32(data+12);
        require(call("NtClose", {event}) == 0xc0000008, "closed source rejected");
        require(call("KeSetEvent", {eventAddress,1,0}) == 0, "direct dispatcher event set");
        require(call("NtWaitForSingleObjectEx", {alias,1,0,data+80}) == 0, "duplicate shares dispatcher signal");
        require(call("NtClose", {alias}) == 0, "duplicate close");
        require(lookupObjectAddress(eventAddress) != nullptr, "reference retains object after handle close");
        call("ObDereferenceObject", {eventAddress});
        require(!guest.valid(eventAddress,1), "last reference releases guest event storage");

        const char name[] = "NativeKernelContractSemaphore";
        memcpy(guest.base()+data+512,name,sizeof(name));
        guest.write32(data+500,(uint32_t(sizeof(name)-1)<<16)|sizeof(name)); guest.write32(data+504,data+512);
        guest.write32(data+480,0xfffffffc); guest.write32(data+484,data+500); guest.write32(data+488,0x80);
        require(call("NtCreateSemaphore", {data+20,data+480,1,2}) == 0, "title namespace semaphore create");
        uint32_t semaphore = guest.read32(data+20);
        require(call("NtCreateSemaphore", {data+24,data+480,0,2}) == 0x40000000, "open existing semaphore status");
        uint32_t semaphoreAlias = guest.read32(data+24);
        require(call("NtWaitForSingleObjectEx", {semaphoreAlias,1,0,data+80}) == 0, "named open preserves original count");
        require(call("NtWaitForSingleObjectEx", {semaphore,1,0,data+80}) == 0x102, "named aliases share count");
        require(call("NtReleaseSemaphore", {semaphore,2,data+28}) == 0 && guest.read32(data+28) == 0, "release previous count");
        require(call("NtReleaseSemaphore", {semaphore,1,0}) == 0xc0000047, "semaphore limit enforced");
        require(call("NtCreateEvent", {data+32,data+480,0,0}) == 0xc0000024, "named object kind mismatch");
        guest.write32(data+488,0);
        require(call("NtCreateSemaphore", {data+32,data+480,0,2}) == 0xc0000035, "existing named object collision");
        call("NtClose", {semaphore}); call("NtClose", {semaphoreAlias});
        guest.write32(data+488,0x80);
        require(call("NtCreateSemaphore", {data+20,data+480,0,2}) == 0, "closed namespace object can be recreated");
        call("NtClose", {guest.read32(data+20)});

        uint32_t embeddedEvent = data+1024;
        guest.write32(embeddedEvent,0x00000400); guest.write32(embeddedEvent+4,0);
        guest.write32(embeddedEvent+8,embeddedEvent+8); guest.write32(embeddedEvent+12,embeddedEvent+8);
        require(call("KeSetEvent", {embeddedEvent,1,0}) == 0, "initialized embedded event");
        require(call("KeWaitForSingleObject", {embeddedEvent,3,1,0,data+80}) == 0, "manual-reset event wait");
        require(call("KeWaitForSingleObject", {embeddedEvent,3,1,0,data+80}) == 0, "manual-reset event remains signaled");
        require(call("KeResetEvent", {embeddedEvent}) == 1, "reset previous state");
        require(call("KeWaitForSingleObject", {embeddedEvent,3,1,0,data+80}) == 0x102, "reset embedded event wait");
        uint32_t legacyEvent = data+1152;
        guest.write32(legacyEvent,0x01000000); guest.write32(legacyEvent+4,1);
        guest.write32(legacyEvent+8,legacyEvent+8); guest.write32(legacyEvent+12,legacyEvent+8); guest.write32(legacyEvent+16,1);
        require(call("KeWaitForSingleObject", {legacyEvent,3,1,0,data+80}) == 0, "original20-byte event factory ABI without size byte");
        call("KeSetEvent", {legacyEvent,1,0}); guest.write32(legacyEvent+4,0);
        require(call("KeWaitForSingleObject", {legacyEvent,3,1,0,data+80}) == 0x102, "original direct signal-field reset reconciled");
        // Force a dispatcher lookup to queue behind an event operation. The
        // lock owner publishes the opposite signal before the contender resumes.
        // A pre-lock guest snapshot would undo that publication and report the
        // wrong previous state for both SetEvent and ResetEvent.
        for (bool setting : {true, false}) {
            call(setting ? "KeResetEvent" : "KeSetEvent", {embeddedEvent,1,0});
            auto object = lookupObjectAddress(embeddedEvent);
            std::unique_lock held(object->dispatcherMutex);
            std::atomic<DWORD> contenderId{0};
            std::atomic<uint32_t> previous{0xffffffff};
            std::atomic<bool> operationFailed{false};
            std::jthread contender([&] {
                PPCContext other{}; other.r3.u64 = embeddedEvent; other.r4.u64 = 1;
                contenderId.store(GetCurrentThreadId());
                try {
                    synchronizationImport(other, guest.base(), setting ? "KeSetEvent" : "KeResetEvent");
                    previous.store(other.r3.u32);
                } catch (...) { operationFailed.store(true); }
            });
            while (!contenderId.load()) SwitchToThread();
            bool blocked = waitForBlockedThread(contenderId.load());
            call(setting ? "KeSetEvent" : "KeResetEvent", {embeddedEvent,1,0});
            held.unlock(); contender.join();
            require(blocked && !operationFailed.load(), "event contender reached dispatcher lock");
            require(previous.load() == uint32_t(setting), "queued event operation uses signal published after it began");
            require(guest.read32(embeddedEvent + 4) == uint32_t(setting) &&
                hostEventState(object->handle) == LONG(setting), "queued event preserves matching guest/native state");
        }
        // Native waits must reserve the event under the same lock used by
        // reconciliation; otherwise they can consume an auto-reset signal while
        // a reconciler still believes there are no active waiters.
        for (bool multiple : {false, true}) {
            require(call("NtCreateEvent", {data+72,0,1,1}) == 0, "wait reservation auto-reset event");
            require(call("NtCreateEvent", {data+76,0,0,0}) == 0, "wait reservation companion event");
            uint32_t token = guest.read32(data+72), companion = guest.read32(data+76);
            guest.write32(data+144, token); guest.write32(data+148, companion);
            auto object = lookupObject(token);
            std::unique_lock held(object->dispatcherMutex);
            std::atomic<DWORD> waiterId{0};
            std::atomic<uint32_t> waited{0xffffffff};
            std::atomic<bool> operationFailed{false};
            std::jthread waiter([&] {
                PPCContext other{};
                if (multiple) {
                    other.r3.u64 = 2; other.r4.u64 = data+144; other.r5.u64 = 1;
                    other.r6.u64 = 1; other.r8.u64 = data+80;
                } else {
                    other.r3.u64 = token; other.r4.u64 = 1; other.r6.u64 = data+80;
                }
                waiterId.store(GetCurrentThreadId());
                try {
                    synchronizationImport(other, guest.base(), multiple ? "NtWaitForMultipleObjectsEx" : "NtWaitForSingleObjectEx");
                    waited.store(other.r3.u32);
                } catch (...) { operationFailed.store(true); }
            });
            while (!waiterId.load()) SwitchToThread();
            bool blocked = waitForBlockedThread(waiterId.load());
            bool reservedUnderLock = object->activeWaiters.load() == 0 && hostEventState(object->handle) == 1;
            held.unlock(); waiter.join();
            require(blocked && !operationFailed.load() && waited.load() == 0, "queued native wait consumes signal after lock is released");
            require(reservedUnderLock, "wait reservation cannot race event reconciliation under its lock");
            require(object->activeWaiters.load() == 0 && lookupObject(companion)->activeWaiters.load() == 0 &&
                guest.read32(object->guestAddress+4) == 0 && hostEventState(object->handle) == 0,
                "completed auto-reset wait publishes consumed signal before releasing reservations");
            call("NtClose", {token}); call("NtClose", {companion});
        }
        bool failed = false;
        try { call("KeSetEvent", {data+1100,1,0}); } catch (const NativeFailure&) { failed = true; }
        require(failed, "uninitialized dispatcher storage must fail");

        require(call("NtCreateEvent", {data+40,0,0,0}) == 0, "multiwait first event");
        require(call("NtCreateEvent", {data+44,0,0,1}) == 0, "multiwait second event");
        guest.write32(data+120,guest.read32(data+40)); guest.write32(data+124,guest.read32(data+44));
        require(call("NtWaitForMultipleObjectsEx", {2,data+120,1,1,0,data+80}) == 1, "wait-any index");
        require(call("NtWaitForMultipleObjectsEx", {2,data+120,0,1,0,data+80}) == 0x102, "wait-all waits for both objects");
        call("NtSetEvent", {guest.read32(data+40),0});
        require(call("NtWaitForMultipleObjectsEx", {2,data+120,0,1,0,data+80}) == 0, "wait-all fulfilled");
        call("NtClose", {guest.read32(data+40)}); call("NtClose", {guest.read32(data+44)});

        require(call("NtCreateEvent", {data+60,0,1,1}) == 0 && call("NtCreateEvent", {data+64,0,1,0}) == 0, "cancellation guest events");
        uint32_t cancelFirst = guest.read32(data+60), cancelSecond = guest.read32(data+64);
        guest.write32(data+128,cancelFirst); guest.write32(data+132,cancelSecond);
        HANDLE cancellation = CreateEventW(nullptr,TRUE,TRUE,nullptr);
        require(cancellation != nullptr, "native callback cancellation event");
        HANDLE previousCancellation = setDispatcherCancellation(cancellation);
        bool cancelled = false;
        try { call("NtWaitForSingleObjectEx", {cancelFirst,1,0,0}); } catch (const DispatcherWaitCancelled&) { cancelled = true; }
        require(cancelled && lookupObject(cancelFirst)->activeWaiters.load() == 0, "cancellation unwinds wait bookkeeping");
        ResetEvent(cancellation);
        require(call("NtWaitForMultipleObjectsEx", {2,data+128,1,1,0,data+80}) == 0, "cancellation preserves guest wait index and signal");
        call("NtSetEvent", {cancelFirst,0});
        std::jthread cancelWait([&] {
            uint64_t deadline = GetTickCount64()+10000;
            while (!lookupObject(cancelSecond)->activeWaiters.load() && GetTickCount64() < deadline) SwitchToThread();
            SetEvent(cancellation);
        });
        cancelled = false;
        try { call("NtWaitForMultipleObjectsEx", {2,data+128,0,1,0,0}); } catch (const DispatcherWaitCancelled&) { cancelled = true; }
        cancelWait.join();
        require(cancelled && lookupObject(cancelFirst)->activeWaiters.load() == 0 && lookupObject(cancelSecond)->activeWaiters.load() == 0, "wait-all cancellation unwinds all objects");
        ResetEvent(cancellation);
        require(call("NtWaitForSingleObjectEx", {cancelFirst,1,0,data+80}) == 0, "cancelled wait-all does not consume partial guest signal");
        guest.write64(data+112,uint64_t(-200000ll));
        require(call("NtWaitForMultipleObjectsEx", {2,data+128,0,1,0,data+112}) == 0x102, "cancellable wait-all preserves relative deadline");
        setDispatcherCancellation(previousCancellation); CloseHandle(cancellation);
        call("NtClose", {cancelFirst}); call("NtClose", {cancelSecond});

        require(call("NtCreateEvent", {data+48,0,0,0}) == 0, "critical-region wait event");
        call("KeEnterCriticalRegion", {}); call("KeEnterCriticalRegion", {});
        require(QueueUserAPC(countApc,GetCurrentThread(),0) != 0, "queue real native APC");
        require(call("NtWaitForSingleObjectEx", {guest.read32(data+48),1,1,data+80}) == 0x102 && apcCount.load() == 0, "critical region defers user APC");
        call("KeLeaveCriticalRegion", {}); require(apcCount.load() == 0, "nested critical region remains disabled");
        call("KeLeaveCriticalRegion", {}); require(apcCount.load() == 1, "outer critical region delivers pending APC");
        call("NtClose", {guest.read32(data+48)});

        require(call("NtCreateTimer", {data+52,0,1}) == 0, "synchronization timer create");
        uint32_t timer = guest.read32(data+52);
        guest.write64(data+96,uint64_t(-50000ll)); guest.write64(data+104,uint64_t(-10000000ll));
        require(call("NtSetTimerEx", {timer,data+96,0,1,0,0,0,0}) == 0, "native relative timer activation");
        require(call("NtWaitForSingleObjectEx", {timer,1,0,data+104}) == 0, "native timer expires");
        require(call("NtWaitForSingleObjectEx", {timer,1,0,data+80}) == 0x102, "synchronization timer signal consumed");
        guest.write64(data+96,uint64_t(-100000000ll));
        require(call("NtSetTimerEx", {timer,data+96,0,1,0,0,0,0}) == 0 && call("NtCancelTimer", {timer,data+56}) == 0, "pending timer cancellation");
        require(call("NtWaitForSingleObjectEx", {timer,1,0,data+80}) == 0x102, "canceled timer remains nonsignaled");
        call("NtClose", {timer});

        uint32_t rw = data+1600;
        call("ExInitializeReadWriteLock", {rw});
        require(guest.read32(rw) == 0xffffffff && guest.read32(rw+16) == 0x01000400 && guest.read32(rw+32) == 0x05000500, "ERWLOCK initialized ABI");
        call("ExAcquireReadWriteLockShared", {rw});
        HANDLE readerReady = CreateEventW(nullptr,TRUE,FALSE,nullptr), readerRelease = CreateEventW(nullptr,TRUE,FALSE,nullptr);
        HANDLE writerReady = CreateEventW(nullptr,TRUE,FALSE,nullptr);
        require(readerReady && readerRelease && writerReady, "native lock test events");
        std::jthread reader([&] {
            PPCContext other{}; other.r3.u64=rw; synchronizationImport(other,guest.base(),"ExAcquireReadWriteLockShared");
            SetEvent(readerReady);
            if (WaitForSingleObject(readerRelease,10000) != WAIT_OBJECT_0) ExitProcess(1);
            other.r3.u64=rw; synchronizationImport(other,guest.base(),"ExReleaseReadWriteLock");
        });
        if (WaitForSingleObject(readerReady,10000) != WAIT_OBJECT_0) { fprintf(stderr,"Shared readers did not acquire concurrently\n"); ExitProcess(1); }
        require(guest.read32(rw+12) == 2, "multiple concurrent native readers");
        std::jthread writer([&] {
            PPCContext other{}; other.r3.u64=rw; synchronizationImport(other,guest.base(),"ExAcquireReadWriteLockExclusive");
            SetEvent(writerReady); other.r3.u64=rw; synchronizationImport(other,guest.base(),"ExReleaseReadWriteLock");
        });
        uint64_t deadline = GetTickCount64()+10000;
        while (guest.read32(rw+4) != 1 && GetTickCount64() < deadline) SwitchToThread();
        if (guest.read32(rw+4) != 1) { fprintf(stderr,"Exclusive writer did not enter wait queue\n"); ExitProcess(1); }
        require(WaitForSingleObject(writerReady,0) == WAIT_TIMEOUT, "exclusive writer excluded by active readers");
        SetEvent(readerRelease); reader.join();
        require(WaitForSingleObject(writerReady,0) == WAIT_TIMEOUT, "remaining reader continues to exclude writer");
        call("ExReleaseReadWriteLock", {rw});
        if (WaitForSingleObject(writerReady,10000) != WAIT_OBJECT_0) { fprintf(stderr,"Exclusive writer did not wake after readers left\n"); ExitProcess(1); }
        writer.join();
        require(guest.read32(rw) == 0xffffffff && guest.read32(rw+4) == 0 && guest.read32(rw+12) == 0, "released native read/write lock state");
        CloseHandle(readerReady); CloseHandle(readerRelease); CloseHandle(writerReady);
        call("ExInitializeReadWriteLock", {rw});

        PPC_LOOKUP_FUNC(guest.base(), PPC_CODE_BASE) = testWorker;
        PPC_LOOKUP_FUNC(guest.base(), PPC_CODE_BASE+4) = testStartup;
        guest.write32(data+2000,PPC_CODE_BASE); guest.write32(data+2004,100);
        call("ExRegisterTitleTerminateNotification", {data+2000,1});
        auto notifications = titleTerminationNotifications();
        require(notifications.size() == 1 && notifications[0].registration == data+2000 && notifications[0].routine == PPC_CODE_BASE && notifications[0].priority == 100, "termination callback registration retained");
        call("ExRegisterTitleTerminateNotification", {data+2000,0});
        require(titleTerminationNotifications().empty(), "termination callback unregister");
        require(call("ExCreateThread", {data+160,0x10000,data+164,PPC_CODE_BASE+4,PPC_CODE_BASE,0x10203040,0x01000001}) == 0, "suspended native worker creation");
        uint32_t worker = guest.read32(data+160), id = guest.read32(data+164);
        require(workerPCR.load() == 0 && liveGuestThreads(), "suspended worker must not execute");
        require(call("ObReferenceObjectByHandle", {worker,threadType,data+168}) == 0, "typed worker reference");
        uint32_t thread = guest.read32(data+168);
        require(call("KeSetBasePriorityThread", {thread,1}) == 0, "previous native guest priority");
        require(call("KeQueryBasePriorityThread", {thread}) == 1, "stored guest priority");
        require(call("KeSetAffinityThread", {thread,2,data+172}) == 0 && guest.read32(data+172) == 1, "native affinity and previous mask");
        require(call("NtResumeThread", {worker,data+176}) == 0 && guest.read32(data+176) == 1, "resume suspend count");
        guest.write64(data+88,uint64_t(-100000000ll));
        require(call("NtWaitForSingleObjectEx", {worker,1,0,data+88}) == 0, "join native worker");
        require(workerPCR.load() != ctx.r13.u32 && workerArgument.load() == 0x10203040, "independent worker PCR and startup argument");
        require(guest.read32(thread+320) == 0x76543210 && (guest.read32(thread+4)&255) == 1, "thread exit publication ABI");
        require(lookupThreadId(id) != nullptr && !liveGuestThreads(), "completed native thread remains referencable");
        call("NtClose", {worker}); call("ObDereferenceObject", {thread});
        shutdownKernelObjects(); currentContext = nullptr;
        fprintf(stderr,"Native synchronization, namespace, object lifetime and worker ABI contracts passed.\n");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr,"Native kernel contract: %s\n",error.what()); return 1;
    }
}
