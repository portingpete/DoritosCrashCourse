#include "title_lifecycle.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>

namespace Doritos::Native {
namespace {
[[noreturn]] void terminateTitle(DWORD status) noexcept {
    // ExitProcess runs DLL detach callbacks after killing other threads, which
    // can deadlock on a lock held by those threads. The title has no complete
    // cooperative guest-thread shutdown yet; retain its mappings until the OS
    // terminates the entire process instead.
    TerminateProcess(GetCurrentProcess(), status);
    ExitProcess(status); // Only reached if the native termination call fails.
}
}
TitleCloseCoordinator::TitleCloseCoordinator(std::function<void()> stopAndFlush, DWORD deadline)
    : stopAndFlush_(std::move(stopAndFlush)), deadline_(deadline) {
    if (!stopAndFlush_ || !deadline_) throw std::invalid_argument("Invalid native title close coordinator");
    requested_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    disarmed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    completed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    try {
        if (!requested_ || !disarmed_ || !completed_) throw std::runtime_error("Cannot create native title close events");
        coordinator_ = std::thread([this] { run(); });
    } catch (...) {
        if (requested_) CloseHandle(requested_);
        if (disarmed_) CloseHandle(disarmed_);
        if (completed_) CloseHandle(completed_);
        throw;
    }
}
TitleCloseCoordinator::~TitleCloseCoordinator() {
    SetEvent(disarmed_);
    if (coordinator_.joinable()) coordinator_.join();
    CloseHandle(completed_); CloseHandle(disarmed_); CloseHandle(requested_);
}
void TitleCloseCoordinator::run() noexcept {
    HANDLE signals[]{disarmed_, requested_};
    DWORD wait = WaitForMultipleObjects(2, signals, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0) return;
    if (wait != WAIT_OBJECT_0 + 1) terminateTitle(1);
    std::atomic<DWORD> result{0};
    try {
        std::thread shutdown([&] {
            try {
                stopAndFlush_();
                fprintf(stderr, "[NATIVE CLOSE] Audio stopped; profile writes finished; terminating guest threads.\n");
            }
            catch (const std::exception& error) {
                fprintf(stderr, "[NATIVE CLOSE] Service stop failed: %s\n", error.what()); result.store(1);
            } catch (...) { result.store(1); }
            // Logging and stdio locks are also covered by the close deadline.
            fflush(stderr); fflush(stdout);
            SetEvent(completed_);
        });
        // A stuck service or callback cannot leave invisible audio running.
        // Completed profile writes were flushed atomically by the user bridge;
        // this deadline also leaves the prior file intact if a disk is stuck.
        if (WaitForSingleObject(completed_, deadline_) != WAIT_OBJECT_0) terminateTitle(2);
        terminateTitle(result.load());
    } catch (...) { terminateTitle(1); }
}
}
