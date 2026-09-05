#pragma once
#include <windows.h>
#include <cstdint>

constexpr uint32_t nr_worker_protocol_magic = 0x4E525743; // NRWC
constexpr uint32_t nr_worker_protocol_version = 8;
constexpr uint32_t nr_worker_max_mask_tiles = 256;

enum TemporalFlags : uint32_t {
    temporal_valid = 1u << 0,
    temporal_scene_cut = 1u << 1,
    temporal_reject_all = 1u << 2
};

struct TemporalAnalysisPayload {
    uint64_t frame_sequence{};
    int32_t camera_motion_x{};
    int32_t camera_motion_y{};
    uint32_t confidence_percent{};
    uint32_t forward_backward_error{};
    uint32_t history_rejected_percent{100};
    uint32_t flags{};
    uint16_t mask_columns{};
    uint16_t mask_rows{};
    uint16_t nr_style{1};
    uint16_t nr_preset{3};
    uint16_t nr_intensity_percent{100};
    uint8_t nr_temporal{1};
    uint8_t nr_automask{1};
    uint8_t rejection_mask[nr_worker_max_mask_tiles]{};
};

struct WorkerTemporalState {
    uint32_t magic{nr_worker_protocol_magic};
    uint32_t version{nr_worker_protocol_version};
    uint32_t byte_size{sizeof(WorkerTemporalState)};
    volatile LONG sequence{};
    volatile LONG64 worker_heartbeat_ms{};
    volatile LONG64 worker_processed_frames{};
    volatile LONG flow_mode{};
    volatile LONG flow_error{};
    volatile LONG64 worker_published_frames{};
    volatile LONG64 worker_dropped_outputs{};
    // Protected by the output texture's keyed mutex, unlike live timing counters.
    uint64_t output_frame_sequence{};
    uint64_t output_processing_us{};
    uint64_t output_completed_ms{};
    volatile LONG64 nr_total_us{};
    volatile LONG64 nr_input_us{};
    volatile LONG64 nr_setup_us{};
    volatile LONG64 nr_optical_flow_us{};
    volatile LONG64 nr_motion_vector_us{};
    volatile LONG64 nr_gpu_prepare_us{};
    volatile LONG64 nr_gpu_execute_us{};
    volatile LONG64 nr_bridge_output_us{};
    volatile LONG64 nr_correction_output_us{};
    volatile LONG worker_adapter_state{};
    volatile LONG worker_adapter_error{};
    volatile LONG nr_enabled{};
    volatile LONG nr_style{1};
    volatile LONG nr_preset{3};
    volatile LONG nr_intensity_percent{100};
    volatile LONG nr_temporal{1};
    wchar_t worker_adapter_name[64]{};
    wchar_t worker_adapter_error_message[256]{};
    TemporalAnalysisPayload payload{};
};
