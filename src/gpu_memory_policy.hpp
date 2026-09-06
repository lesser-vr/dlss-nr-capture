#pragma once
#include <cstdint>

// DXGI budgets and usage belong to the worker process, not the whole GPU.
inline bool gpu_memory_pressure(uint64_t usage, uint64_t budget,
                                uint64_t sampled_ms, uint64_t now_ms) noexcept {
    return budget && sampled_ms && now_ms >= sampled_ms && now_ms - sampled_ms <= 3000 &&
        usage >= budget - budget / 10;
}
