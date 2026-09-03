#pragma once

#include <cstdint>
#include <vector>

struct VideoFrame {
    uint32_t width{};
    uint32_t height{};
    int64_t timestamp_100ns{};
    uint64_t arrival_tick_ms{};
    uint64_t sequence{};
    std::vector<uint8_t> bgra;
};
