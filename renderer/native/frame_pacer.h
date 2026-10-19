#pragma once
#include <chrono>
#include <cstdint>
#include <thread>
#include <windows.h>

namespace Doritos::Graphics {
// Keep the clock phase through ordinary scheduler jitter, but never follow a
// missed whole frame with a burst of submissions to recover the old deadline.
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    Time deadline(Time now, uint32_t limit) {
        if (!started_ || !limit) next_ = now;
        else if (limit != limit_) next_ = last_ + period(limit);
        limit_ = limit;
        return next_;
    }
    // Call after success, using submission time for blocking VSync or
    // completion time for immediate presentation.
    void presented(Time now) {
        last_ = now;
        started_ = true;
        if (!limit_) { next_ = now; return; }
        const auto interval = period(limit_);
        const auto scheduled = next_ + interval;
        next_ = scheduled > now ? scheduled : now + interval;
    }
    void reset() { started_ = false; limit_ = 0; }
private:
    static std::chrono::nanoseconds period(uint32_t limit) { return std::chrono::nanoseconds(1000000000ULL / limit); }
    Time last_{}, next_{};
    uint32_t limit_ = 0;
    bool started_ = false;
};

// A per-renderer timer avoids changing the process/system timer resolution.
// Older Windows versions and timer failures retain a normal sleeping fallback.
class FrameWaiter {
public:
    FrameWaiter() = default;
    FrameWaiter(const FrameWaiter&) = delete;
    FrameWaiter& operator=(const FrameWaiter&) = delete;
    ~FrameWaiter() { if (timer_) CloseHandle(timer_); }
    void waitUntil(FramePacer::Time deadline) {
        auto now = FramePacer::Clock::now();
        if (now >= deadline) return;
        if (!attempted_) {
            attempted_ = true;
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
        while (now < deadline) {
            if (!timer_) { std::this_thread::sleep_until(deadline); return; }
            const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
            LARGE_INTEGER due{};
            due.QuadPart = -((remaining + 99) / 100);
            if (!SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0) ||
                WaitForSingleObject(timer_, INFINITE) != WAIT_OBJECT_0) {
                std::this_thread::sleep_until(deadline);
                return;
            }
            now = FramePacer::Clock::now();
        }
    }
private:
    HANDLE timer_ = nullptr;
    bool attempted_ = false;
};
}
