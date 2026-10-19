#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace Doritos::Graphics {
struct FrameTimeSummary {
    uint32_t intervals = 0;
    uint64_t nanoseconds = 0;
    uint64_t worstNanoseconds = 0;
    uint64_t medianNanoseconds = 0, p95Nanoseconds = 0, p99Nanoseconds = 0;
};
// Only successful, consecutive drawn Present timestamps enter this window.
// Clear-only presentation interrupts the sequence and resets it.
class RecentFrameTimes {
public:
    static constexpr uint32_t capacity = 300;
    void record(uint64_t nanoseconds) {
        intervals_[next_] = nanoseconds;
        next_ = (next_ + 1) % capacity;
        count_ = std::min(count_ + 1, capacity);
    }
    void reset() { next_ = count_ = 0; }
    FrameTimeSummary summary() const {
        FrameTimeSummary result{count_};
        for (uint32_t index = 0; index < count_; ++index) {
            result.nanoseconds += intervals_[index];
            result.worstNanoseconds = std::max(result.worstNanoseconds, intervals_[index]);
        }
        if (count_) {
            auto sorted = intervals_;
            std::sort(sorted.begin(), sorted.begin() + count_);
            // Nearest-rank percentiles of real intervals, without zero padding.
            result.medianNanoseconds = sorted[(count_ * 50 + 99) / 100 - 1];
            result.p95Nanoseconds = sorted[(count_ * 95 + 99) / 100 - 1];
            result.p99Nanoseconds = sorted[(count_ * 99 + 99) / 100 - 1];
        }
        return result;
    }
private:
    std::array<uint64_t, capacity> intervals_{};
    uint32_t next_ = 0, count_ = 0;
};
}
