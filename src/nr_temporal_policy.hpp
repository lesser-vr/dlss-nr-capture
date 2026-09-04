#pragma once
#include "worker_protocol.hpp"

struct NrTemporalPolicy {
    bool reset_history;
    bool use_motion_vectors;
};

constexpr NrTemporalPolicy nr_temporal_policy(const TemporalAnalysisPayload& frame) noexcept
{
    // A history reset must not change the feature's allocation/creation mode.
    return {(frame.flags & temporal_scene_cut) != 0, frame.nr_temporal != 0};
}
