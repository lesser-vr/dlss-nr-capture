#pragma once
#include <cstdint>
#include <algorithm>

struct NrSpeedPolicy {
    uint64_t enable_us;
    uint64_t slow_us;
    uint32_t fast_samples;
    uint32_t slow_samples;
};

struct NrWarmupGate {
    uint64_t first_result_ms{};
    bool started{};
    bool finished{};

    bool accept_sample(uint64_t now_ms) noexcept {
        if (finished) return true;
        if (!started) {
            started = true;
            first_result_ms = now_ms;
            return false;
        }
        finished = now_ms >= first_result_ms && now_ms - first_result_ms >= 2000;
        return finished;
    }
};

constexpr NrSpeedPolicy nr_speed_policy(uint32_t numerator, uint32_t denominator) noexcept
{
    // Reject invalid or unreasonable driver rates; retain the tested 60 Hz default.
    if (!numerator || !denominator || numerator < denominator ||
        static_cast<uint64_t>(numerator) > 1000ull * denominator)
        return {15000, 25000, 30, 60};
    const uint64_t n = numerator, d = denominator;
    return {
        900000ull * d / n,
        1500000ull * d / n,
        static_cast<uint32_t>((n + 2 * d - 1) / (2 * d)),
        static_cast<uint32_t>((n + d - 1) / d)
    };
}

// One observation per completed worker result, never per display frame.
class NrSpeedMonitor {
public:
    void reset(uint32_t numerator, uint32_t denominator) noexcept {
        policy_ = nr_speed_policy(numerator, denominator);
        warmup_ = {};
        ema_us_ = 0;
        fast_count_ = slow_count_ = 0;
        fast_ = slow_ = false;
    }

    void observe(uint64_t now_ms, uint64_t processing_us) noexcept {
        if (!processing_us || !warmup_.accept_sample(now_ms)) return;
        ema_us_ = ema_us_ ? (ema_us_ * 7 + processing_us) / 8 : processing_us;
        if (!fast_) {
            if (ema_us_ <= policy_.enable_us) {
                fast_count_ = std::min(fast_count_ + 1, policy_.fast_samples);
                slow_count_ = 0;
                if (fast_count_ >= policy_.fast_samples) {
                    fast_ = true;
                    slow_ = false;
                }
            } else {
                fast_count_ = 0;
                if (ema_us_ >= policy_.slow_us) {
                    slow_count_ = std::min(slow_count_ + 1, policy_.slow_samples);
                    if (slow_count_ >= policy_.slow_samples) slow_ = true;
                } else {
                    slow_count_ = 0;
                }
            }
        } else if (ema_us_ >= policy_.slow_us) {
            slow_count_ = std::min(slow_count_ + 1, policy_.slow_samples);
            if (slow_count_ >= policy_.slow_samples) {
                fast_ = false;
                slow_ = true;
                fast_count_ = 0;
            }
        } else {
            slow_count_ = 0;
        }
    }

    bool fast() const noexcept { return fast_; }
    bool slow() const noexcept { return slow_; }
    bool preparing() const noexcept { return !fast_ && !slow_; }
    uint64_t average_us() const noexcept { return ema_us_; }

private:
    NrSpeedPolicy policy_{nr_speed_policy(60, 1)};
    NrWarmupGate warmup_{};
    uint64_t ema_us_{};
    uint32_t fast_count_{}, slow_count_{};
    bool fast_{}, slow_{};
};
