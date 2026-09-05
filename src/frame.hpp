#pragma once

#include <cstdint>
#include <vector>
#include "worker_protocol.hpp"
#include "gpu_capture.hpp"

struct VideoFrame {
    uint32_t width{};
    uint32_t height{};
    int64_t timestamp_100ns{};
    uint64_t arrival_tick_ms{};
    uint64_t arrival_qpc_100ns{};
    uint64_t sequence{};
    std::vector<uint8_t> bgra;
    TemporalAnalysisPayload temporal;
    std::shared_ptr<GpuCaptureSurface> gpu;
    uint32_t analysis_height{};
    bool gpu_flipped{};
};
