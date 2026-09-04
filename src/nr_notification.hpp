#pragma once
#include <cstdint>

constexpr float nr_notification_opacity(uint64_t elapsed_ms) noexcept
{
    if (elapsed_ms <= 1000) return 1.0f;
    if (elapsed_ms >= 1500) return 0.0f;
    return static_cast<float>(1500 - elapsed_ms) / 500.0f;
}
