#pragma once
#include <cstdint>

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
