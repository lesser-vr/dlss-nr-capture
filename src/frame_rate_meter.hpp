#pragma once
#include <cstdint>

// One-second counter deltas, independent of the configured capture frame rate.
class FrameRateMeter {
public:
    bool observe(uint64_t now_ms, uint64_t frames) noexcept {
        if (!started_ || now_ms < started_ms_ || frames < started_frames_) {
            started_ = true;
            started_ms_ = now_ms;
            started_frames_ = frames;
            fps_ = 0;
            return false;
        }
        const uint64_t elapsed = now_ms - started_ms_;
        if (elapsed < 1000) return false;
        fps_ = static_cast<double>(frames - started_frames_) * 1000.0 / elapsed;
        started_ms_ = now_ms;
        started_frames_ = frames;
        return true;
    }
    double fps() const noexcept { return fps_; }
private:
    bool started_{};
    uint64_t started_ms_{}, started_frames_{};
    double fps_{};
};
