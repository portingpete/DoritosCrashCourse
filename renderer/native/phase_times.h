#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <algorithm>

namespace Doritos::Graphics {
enum class RenderPhase : size_t { Validation, Constants, ConstantDecode, Draw, Scene, Resolve, PresentationCopy, Present, Pacing, CaptureRead, CaptureWrite, BufferUpload, Count };
inline constexpr std::array<const char*,static_cast<size_t>(RenderPhase::Count)> renderPhaseNames{
    "validation","constants","constant_decode","draw_api","scene_api","resolve_api","presentation_copy","present_api","pacing","capture_read","capture_write","buffer_upload"};
struct PhaseSample { uint64_t calls = 0, nanoseconds = 0, worstNanoseconds = 0; };
struct PhaseFrame {
    std::array<PhaseSample,static_cast<size_t>(RenderPhase::Count)> phases{};
    uint64_t bridgeNanoseconds = 0, wallNanoseconds = 0;
};
class PhaseScope {
    PhaseSample* sample_;
    std::chrono::steady_clock::time_point start_;
public:
    PhaseScope(PhaseFrame& frame, RenderPhase phase, bool enabled)
        : sample_(enabled ? &frame.phases[static_cast<size_t>(phase)] : nullptr) {
        if (sample_) start_ = std::chrono::steady_clock::now();
    }
    void stop() {
        if (!sample_) return;
        const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_).count());
        ++sample_->calls;
        sample_->nanoseconds += elapsed;
        sample_->worstNanoseconds = std::max(sample_->worstNanoseconds,elapsed);
        sample_ = nullptr;
    }
    ~PhaseScope() { stop(); }
};
class RecentPhaseTimes {
    std::array<PhaseFrame,300> frames_{};
    uint32_t next_ = 0, count_ = 0;
public:
    static constexpr uint32_t capacity = 300;
    void reset() { next_ = count_ = 0; }
    void record(const PhaseFrame& frame) {
        frames_[next_] = frame;
        next_ = (next_ + 1) % capacity;
        count_ = std::min(count_ + 1,capacity);
    }
    uint32_t count() const { return count_; }
    PhaseFrame summary() const {
        PhaseFrame result;
        for (uint32_t index = 0; index < count_; ++index) {
            const auto& frame = frames_[index];
            result.bridgeNanoseconds += frame.bridgeNanoseconds;
            result.wallNanoseconds += frame.wallNanoseconds;
            for (size_t phase = 0; phase < result.phases.size(); ++phase) {
                auto& destination = result.phases[phase];
                const auto& source = frame.phases[phase];
                destination.calls += source.calls;
                destination.nanoseconds += source.nanoseconds;
                destination.worstNanoseconds = std::max(destination.worstNanoseconds,source.worstNanoseconds);
            }
        }
        return result;
    }
};
}
