#pragma once
#include "frame.hpp"
#include "worker_protocol.hpp"
#include <memory>
#include <string>
#include <string_view>

class IFrameProcessor {
public:
    virtual ~IFrameProcessor() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual bool process(VideoFrame& frame) = 0;
    virtual void reset_history() noexcept = 0;
    virtual std::string diagnostics() const = 0;
    virtual void set_debug_overlay(bool enabled) noexcept = 0;
    virtual TemporalAnalysisPayload temporal_state() const = 0;
};

std::unique_ptr<IFrameProcessor> create_passthrough_processor();
std::unique_ptr<IFrameProcessor> create_motion_analysis_processor();
