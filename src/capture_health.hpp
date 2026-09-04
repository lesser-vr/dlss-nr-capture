#pragma once
#include <cstdint>
constexpr bool capture_input_interrupted(uint64_t now, uint64_t started, uint64_t last_frame,
                                         bool failed) noexcept {
    const uint64_t progress = last_frame ? last_frame : started;
    const uint64_t grace = last_frame ? 2000 : 8000;
    return failed || (now >= progress && now - progress >= grace);
}
constexpr bool capture_retry_due(uint64_t now, uint64_t started, uint64_t last_frame,
                                 uint64_t last_retry, bool failed) noexcept {
    if (now < last_retry || now - last_retry < 5000) return false;
    const uint64_t progress = last_frame ? last_frame : started;
    return failed || (now >= progress && now - progress >= 8000);
}
