#include "frame_processor.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace {
class PassthroughProcessor final : public IFrameProcessor {
public:
    std::string_view name() const noexcept override { return "Passthrough"; }
    bool process(VideoFrame& frame) override { sequence_.store(frame.sequence); return true; }
    void reset_history() noexcept override {}
    std::string diagnostics() const override { return {}; }
    void set_debug_overlay(bool) noexcept override {}
    TemporalAnalysisPayload temporal_state() const override {
        TemporalAnalysisPayload payload{};
        payload.frame_sequence = sequence_.load();
        payload.flags = temporal_valid | temporal_reject_all;
        payload.history_rejected_percent = 100;
        return payload;
    }
private: std::atomic_uint64_t sequence_{};
};

class MotionAnalysisProcessor final : public IFrameProcessor {
public:
    std::string_view name() const noexcept override { return "Motion Analysis"; }

    bool process(VideoFrame& frame) override
    {
        constexpr uint32_t analysis_width = 96;
        constexpr int tile_size = 8;
        const uint32_t pixel_width = frame.gpu ? 96 : frame.width;
        const uint32_t pixel_height = frame.gpu ? frame.analysis_height : frame.height;
        if (!pixel_width || !pixel_height || frame.bgra.size() < static_cast<size_t>(pixel_width) * pixel_height * 4)
            return false;
        const uint32_t width = std::min(analysis_width, frame.width);
        const uint32_t height = std::max(1u, static_cast<uint32_t>(
            std::lround(static_cast<double>(frame.height) * width / std::max(1u, frame.width))));
        std::vector<uint8_t> current(static_cast<size_t>(width) * height);
        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t sy = std::min(pixel_height - 1, y * pixel_height / height);
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t sx = std::min(pixel_width - 1, x * pixel_width / width);
                const uint8_t* p = frame.bgra.data() + (static_cast<size_t>(sy) * pixel_width + sx) * 4;
                current[static_cast<size_t>(y) * width + x] =
                    static_cast<uint8_t>((29 * p[0] + 150 * p[1] + 77 * p[2]) >> 8);
            }
        }

        if (previous_.size() != current.size() || previous_width_ != width || previous_height_ != height) {
            // Never expose a valid payload from the previous capture session.
            {
                std::scoped_lock lock(payload_mutex_);
                payload_ = {};
                payload_.frame_sequence = frame.sequence;
            }
            previous_ = std::move(current);
            previous_width_ = width;
            previous_height_ = height;
            ready_.store(false);
            return true;
        }

        struct Shift { int x{}, y{}; double best{}, second{}; };
        auto find_shift = [width, height](const std::vector<uint8_t>& first,
                                          const std::vector<uint8_t>& second_image) {
            Shift result{};
            result.best = result.second = std::numeric_limits<double>::max();
            constexpr int radius = 6;
            for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx) {
                uint64_t difference = 0; uint32_t samples = 0;
                const int x0 = std::max(0, -dx), x1 = std::min<int>(width, width - dx);
                const int y0 = std::max(0, -dy), y1 = std::min<int>(height, height - dy);
                for (int y = y0; y < y1; y += 2) for (int x = x0; x < x1; x += 2) {
                    difference += std::abs(static_cast<int>(first[static_cast<size_t>(y) * width + x]) -
                                           static_cast<int>(second_image[static_cast<size_t>(y + dy) * width + x + dx]));
                    ++samples;
                }
                if (!samples) continue;
                const double score = static_cast<double>(difference) / samples;
                if (score < result.best) {
                    result.second = result.best; result.best = score; result.x = dx; result.y = dy;
                } else if (score < result.second) result.second = score;
            }
            return result;
        };

        const Shift forward = find_shift(current, previous_);
        const Shift backward = find_shift(previous_, current);
        const int fb_error = std::abs(forward.x + backward.x) + std::abs(forward.y + backward.y);
        double confidence = forward.second < std::numeric_limits<double>::max()
            ? std::clamp((forward.second - forward.best) / std::max(1.0, forward.second) * 4.0, 0.0, 1.0) : 0.0;
        confidence *= std::max(0.0, 1.0 - fb_error / 4.0);
        uint32_t current_histogram[16]{}, previous_histogram[16]{};
        for (size_t i = 0; i < current.size(); ++i) {
            ++current_histogram[current[i] >> 4];
            ++previous_histogram[previous_[i] >> 4];
        }
        uint64_t histogram_delta = 0;
        for (size_t i = 0; i < 16; ++i)
            histogram_delta += static_cast<uint64_t>(
                std::abs(static_cast<int64_t>(current_histogram[i]) -
                         static_cast<int64_t>(previous_histogram[i])));
        const double histogram_change = current.empty() ? 1.0 :
            static_cast<double>(histogram_delta) / (2.0 * current.size());
        // A large pixel mismatch alone also occurs during fast camera rotation.
        // Hard cuts require a strong global histogram change. Menu overlays are
        // detected separately: they change a substantial part of the image while
        // the best global camera translation remains near zero.
        const int translation = std::abs(forward.x) + std::abs(forward.y);
        const bool hard_cut = forward.best > 42.0 && histogram_change > 0.30;
        const bool overlay_transition = forward.best > 18.0 &&
            histogram_change > 0.08 && translation <= 1 && fb_error <= 1;
        if (transition_cooldown_ > 0) --transition_cooldown_;
        const bool cut = transition_cooldown_ == 0 &&
            (hard_cut || overlay_transition);
        if (cut) transition_cooldown_ = 30;

        const int tile_columns = (static_cast<int>(width) + tile_size - 1) / tile_size;
        const int tile_rows = (static_cast<int>(height) + tile_size - 1) / tile_size;
        std::vector<uint8_t> rejected(static_cast<size_t>(tile_columns) * tile_rows);
        uint32_t rejected_count = 0;
        const bool reject_globally = cut || fb_error > 2 || confidence < 0.10;
        for (int ty = 0; ty < tile_rows; ++ty) for (int tx = 0; tx < tile_columns; ++tx) {
            uint32_t difference = 0, samples = 0;
            for (int y = ty * tile_size; y < std::min((ty + 1) * tile_size, static_cast<int>(height)); ++y)
                for (int x = tx * tile_size; x < std::min((tx + 1) * tile_size, static_cast<int>(width)); ++x) {
                    const int px = x + forward.x, py = y + forward.y;
                    if (px < 0 || py < 0 || px >= static_cast<int>(width) || py >= static_cast<int>(height)) continue;
                    difference += std::abs(static_cast<int>(current[static_cast<size_t>(y) * width + x]) -
                                           static_cast<int>(previous_[static_cast<size_t>(py) * width + px]));
                    ++samples;
                }
            const bool tile_rejected = reject_globally || !samples ||
                static_cast<double>(difference) / samples > 24.0;
            rejected[static_cast<size_t>(ty) * tile_columns + tx] = tile_rejected ? 1 : 0;
            if (tile_rejected) ++rejected_count;
        }

        {
            std::scoped_lock lock(payload_mutex_);
            payload_ = {};
            payload_.frame_sequence = frame.sequence;
            payload_.camera_motion_x = static_cast<int>(std::lround(-forward.x * static_cast<double>(frame.width) / width));
            payload_.camera_motion_y = static_cast<int>(std::lround(-forward.y * static_cast<double>(frame.height) / height));
            payload_.confidence_percent = static_cast<uint32_t>(std::lround(confidence * 100.0));
            payload_.forward_backward_error = static_cast<uint32_t>(fb_error);
            payload_.history_rejected_percent = tile_columns * tile_rows
                ? rejected_count * 100u / static_cast<uint32_t>(tile_columns * tile_rows) : 100u;
            payload_.flags = temporal_valid | (cut ? temporal_scene_cut : 0u) |
                (reject_globally ? temporal_reject_all : 0u);
            payload_.mask_columns = static_cast<uint16_t>(tile_columns);
            payload_.mask_rows = static_cast<uint16_t>(tile_rows);
            std::memcpy(payload_.rejection_mask, rejected.data(),
                        std::min(rejected.size(), static_cast<size_t>(nr_worker_max_mask_tiles)));
        }

        if (debug_overlay_.load() && !frame.gpu) {
            for (uint32_t y = 0; y < frame.height; ++y) for (uint32_t x = 0; x < frame.width; ++x) {
                const int ax = std::min<int>(width - 1, x * width / frame.width);
                const int ay = std::min<int>(height - 1, y * height / frame.height);
                if (!rejected[static_cast<size_t>(ay / tile_size) * tile_columns + ax / tile_size]) continue;
                uint8_t* p = frame.bgra.data() + (static_cast<size_t>(y) * frame.width + x) * 4;
                p[0] = static_cast<uint8_t>(p[0] / 2);
                p[1] = static_cast<uint8_t>(p[1] / 2);
                p[2] = static_cast<uint8_t>(std::min(255, static_cast<int>(p[2] / 2) + 112));
            }
        }

        motion_x_.store(static_cast<int>(std::lround(-forward.x * static_cast<double>(frame.width) / width)));
        motion_y_.store(static_cast<int>(std::lround(-forward.y * static_cast<double>(frame.height) / height)));
        confidence_percent_.store(static_cast<int>(std::lround(confidence * 100.0)));
        history_rejected_percent_.store(tile_columns * tile_rows
            ? static_cast<int>(rejected_count * 100 / (tile_columns * tile_rows)) : 100);
        fb_error_.store(fb_error);
        scene_cut_.store(cut);
        ready_.store(true);
        previous_ = std::move(current);
        return true;
    }

    void reset_history() noexcept override {
        {
            std::scoped_lock lock(payload_mutex_);
            payload_ = {};
        }
        previous_.clear(); previous_width_ = previous_height_ = 0;
        transition_cooldown_ = 0;
        ready_.store(false); scene_cut_.store(false);
    }
    void set_debug_overlay(bool enabled) noexcept override { debug_overlay_.store(enabled); }
    TemporalAnalysisPayload temporal_state() const override {
        std::scoped_lock lock(payload_mutex_);
        return payload_;
    }

    std::string diagnostics() const override {
        if (!ready_.load()) return "warming up";
        char text[160]{};
        std::snprintf(text, sizeof(text),
            "camera %+d,%+d px | confidence %d%% | FB %d | history rejected %d%%%s",
            motion_x_.load(), motion_y_.load(), confidence_percent_.load(), fb_error_.load(),
            history_rejected_percent_.load(), scene_cut_.load() ? " | CUT" : "");
        return text;
    }

private:
    std::vector<uint8_t> previous_;
    uint32_t previous_width_{}, previous_height_{};
    uint32_t transition_cooldown_{};
    std::atomic_int motion_x_{}, motion_y_{}, confidence_percent_{};
    std::atomic_int history_rejected_percent_{}, fb_error_{};
    std::atomic_bool scene_cut_{}, ready_{}, debug_overlay_{};
    mutable std::mutex payload_mutex_;
    TemporalAnalysisPayload payload_{};
};
}

std::unique_ptr<IFrameProcessor> create_passthrough_processor() { return std::make_unique<PassthroughProcessor>(); }
std::unique_ptr<IFrameProcessor> create_motion_analysis_processor() { return std::make_unique<MotionAnalysisProcessor>(); }
