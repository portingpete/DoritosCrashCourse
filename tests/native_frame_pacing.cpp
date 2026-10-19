#include "renderer/native/frame_pacer.h"
#include "renderer/native/recent_frame_times.h"
#include <cstdio>
#include <stdexcept>

using namespace Doritos::Graphics;
using namespace std::chrono_literals;
namespace {
unsigned checks = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
}
int main() {
    try {
        FramePacer pacer;
        const FramePacer::Time origin{};
        require(pacer.deadline(origin, 100) == origin, "First frame must not wait");
        pacer.presented(origin);
        require(pacer.deadline(origin + 2ms, 100) == origin + 10ms, "Normal cap deadline is incorrect");
        pacer.presented(origin + 10ms + 100us);
        require(pacer.deadline(origin + 12ms, 100) == origin + 20ms, "Small scheduler jitter accumulated as drift");
        pacer.presented(origin + 55ms); // Miss multiple periods, e.g. loading/Present stall.
        require(pacer.deadline(origin + 56ms, 100) == origin + 65ms, "Late frame caused an immediate catch-up submission");
        pacer.presented(origin + 65ms);
        require(pacer.deadline(origin + 66ms, 50) == origin + 85ms, "Background cap change used stale foreground deadline");
        pacer.presented(origin + 85ms);
        require(pacer.deadline(origin + 86ms, 200) == origin + 90ms, "Foreground cap change retained background delay");
        pacer.presented(origin + 90ms);
        require(pacer.deadline(origin + 91ms, 0) == origin + 91ms, "Unlimited mode retained a sleeping deadline");
        pacer.presented(origin + 91ms);
        require(pacer.deadline(origin + 92ms, 100) == origin + 101ms, "Returning from unlimited mode did not use the new cap");
        pacer.reset();
        require(pacer.deadline(origin + 93ms, 50) == origin + 93ms, "Reset retained a previous presentation epoch");
        pacer.presented(origin + 93ms);
        pacer.deadline(origin + 94ms, 50);
        pacer.presented(origin + 133ms); // Exactly the next deadline is also expired.
        require(pacer.deadline(origin + 134ms, 50) == origin + 153ms, "Exact missed deadline allowed a burst");

        // With VSync the caller anchors to submission, while the driver's
        // Present wait supplies output spacing. A full driver interval must
        // not be followed by another software interval.
        FramePacer synchronized;
        synchronized.deadline(origin, 100);
        synchronized.presented(origin);
        const auto afterDriverWait = origin + 10ms;
        require(synchronized.deadline(afterDriverWait, 100) <= afterDriverWait,
            "VSync driver wait was counted twice");
        synchronized.presented(afterDriverWait);
        require(synchronized.deadline(origin + 20ms, 100) == origin + 20ms,
            "VSync submissions did not retain one interval of spacing");

        // Tail latency must expose stutter hidden by an average. Values are
        // deliberately inserted out of order and wrap the 300-frame window.
        RecentFrameTimes recent;
        auto summary = recent.summary();
        require(!summary.p99Nanoseconds && !summary.medianNanoseconds, "Empty window has percentiles");
        recent.record(999999999);
        for (unsigned value = 300; value; --value) recent.record(value * 1000000ULL);
        summary = recent.summary();
        require(summary.intervals == 300 && summary.medianNanoseconds == 150000000 &&
            summary.p95Nanoseconds == 285000000 && summary.p99Nanoseconds == 297000000 &&
            summary.worstNanoseconds == 300000000, "Wrapped percentile ranks retained expired samples or zero padding");
        recent.reset();
        recent.record(17000000);
        summary = recent.summary();
        require(summary.medianNanoseconds == 17000000 && summary.p95Nanoseconds == 17000000 &&
            summary.p99Nanoseconds == 17000000, "Partial percentile window used missing samples");

        FrameWaiter waiter;
        const auto before = FramePacer::Clock::now();
        waiter.waitUntil(before - 1ms);
        const auto deadline = FramePacer::Clock::now() + 2ms;
        waiter.waitUntil(deadline);
        require(FramePacer::Clock::now() >= deadline, "Frame wait returned before its deadline");
        std::printf("Native frame pacing: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Frame pacing failure: %s\n", error.what());
        return 1;
    }
}
