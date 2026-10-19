#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

struct PPCContext;

namespace Doritos::Native {
class Memory;
struct NativeFileState;
struct NativeNotificationState;
enum class ObjectKind { File, Directory, Event, Semaphore, Thread, Mutant, Timer, Notification };
struct KernelObject {
    HANDLE handle = nullptr;
    ObjectKind kind = ObjectKind::Event;
    uint32_t guestAddress = 0;
    DWORD threadId = 0;
    std::atomic<uint32_t> affinity{0x3f};
    std::atomic<int32_t> priority{0};
    std::atomic<uint32_t> activeWaiters{0};
    std::recursive_mutex dispatcherMutex;
    bool manualReset = false;
    bool worker = false;
    Memory* owner = nullptr;
    std::vector<uint32_t> allocations;
    std::shared_ptr<NativeFileState> fileState;
    std::shared_ptr<NativeNotificationState> notificationState;
    ~KernelObject();
};
uint32_t registerObject(HANDLE handle, ObjectKind kind, uint32_t guestAddress = 0);
std::shared_ptr<KernelObject> lookupObject(uint32_t handle);
std::shared_ptr<KernelObject> lookupObjectAddress(uint32_t address);
std::shared_ptr<KernelObject> lookupThreadId(DWORD id);
bool closeObject(uint32_t handle);
uint32_t duplicateObject(uint32_t handle);
void retainObject(const std::shared_ptr<KernelObject>& object);
bool releaseObject(uint32_t address);
bool liveGuestThreads();
void configureObjectType(ObjectKind kind, uint32_t address);
void registerMainThread(::PPCContext& ctx);
struct TitleTerminationNotification { uint32_t registration, routine, priority; };
std::vector<TitleTerminationNotification> titleTerminationNotifications();
// Must run before unmapping guest memory. A live worker prevents safe teardown;
// fail the native process rather than racing it against freed guest storage.
void shutdownKernelObjects();
bool synchronizationImport(::PPCContext& ctx, uint8_t* base, const char* name);
// The caller retains the actual native recursive-mutex registry and locking.
// These helpers validate one complete 28-byte CPU object per operation, then
// update its existing big-endian state. PCR ownership remains a separate
// checked memory read; an invalid owner/depth never permits native unlock.
void validateInitialGuestCriticalSection(uint32_t address);
void enterGuestCriticalSection(uint32_t address, uint32_t pcrAddress);
void leaveGuestCriticalSection(uint32_t address, uint32_t pcrAddress);
enum class CriticalSectionOperation { Enter, TryEnter, Leave };
// Default disabled. DORITOS_KERNEL_TIMING=1 reports at most twelve 10,000-call
// batches per host thread, measuring CPU wall calls including real lock waits.
class CriticalSectionCallTiming {
    uint64_t started_ = 0;
    CriticalSectionOperation operation_;
public:
    explicit CriticalSectionCallTiming(CriticalSectionOperation operation) noexcept;
    ~CriticalSectionCallTiming() noexcept;
    CriticalSectionCallTiming(const CriticalSectionCallTiming&) = delete;
    CriticalSectionCallTiming& operator=(const CriticalSectionCallTiming&) = delete;
};
struct GuestThreadExit { uint32_t code; };
// Host-owned callbacks may opt into teardown cancellation. The borrowed manual
// reset event must remain valid until that thread restores its previous value.
// Cancellation unwinds the host callback; it is never returned as a guest status.
struct DispatcherWaitCancelled {};
HANDLE setDispatcherCancellation(HANDLE event);
}
