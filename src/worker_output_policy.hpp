#pragma once
#include "worker_protocol.hpp"

constexpr bool worker_frame_eligible(const TemporalAnalysisPayload& frame,
                                     uint64_t last_sequence) noexcept
{
    return (frame.flags & temporal_valid) != 0 && frame.frame_sequence >= last_sequence;
}

// Key 0 returns ownership to the producer; key 1 publishes to the renderer.
constexpr uint64_t worker_output_release_key(bool processing_succeeded) noexcept
{
    return processing_succeeded ? 1 : 0;
}
