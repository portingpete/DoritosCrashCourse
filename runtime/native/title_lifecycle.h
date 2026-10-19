#pragma once
#include <windows.h>
#include <functional>
#include <thread>

namespace Doritos::Native {
// The window only signals this event. A separate coordinator stops audio and
// waits for synchronous profile persistence before terminating all guest
// threads together, without unmapping memory or running static destructors.
class TitleCloseCoordinator {
    HANDLE requested_ = nullptr, disarmed_ = nullptr, completed_ = nullptr;
    std::function<void()> stopAndFlush_;
    DWORD deadline_;
    std::thread coordinator_;
    void run() noexcept;
public:
    explicit TitleCloseCoordinator(std::function<void()> stopAndFlush, DWORD deadline = 5000);
    ~TitleCloseCoordinator();
    TitleCloseCoordinator(const TitleCloseCoordinator&) = delete;
    TitleCloseCoordinator& operator=(const TitleCloseCoordinator&) = delete;
    HANDLE event() const { return requested_; }
};
}
