#pragma once

#include "capture_engine.hpp"
#include "audio_capture.hpp"
#include "d3d11_renderer.hpp"
#include "frame_processor.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

class App {
public:
    int run(HINSTANCE instance, int show_command);

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK combo_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void discover_capture_devices();
    void start_capture_device(size_t index);
    void start_capture_mode(size_t index);
    void start_capture_format(size_t index);
    void start_capture_resolution(size_t index);
    void start_capture_frame_rate(size_t index);
    void start_audio_capture(size_t index);
    void rebuild_mode_menu();
    bool handle_mode_wheel(WPARAM wparam);
    void set_vertical_flip(bool enabled);
    void toggle_fullscreen();
    void set_always_on_top(bool enabled);
    void set_auto_size_to_resolution(bool enabled);
    void resize_window_to_capture(UINT dpi = 0, const POINT* position = nullptr);
    void load_settings();
    void save_settings();
    void set_motion_analysis(bool enabled);
    void set_history_overlay(bool enabled);
    void set_nr_enabled(bool enabled);
    void apply_nr_settings(bool restart_worker);
    void restart_nr_worker(uint32_t width, uint32_t height);
    void stop_nr_worker();
    std::wstring nr_worker_status() const;
    void ensure_nr_worker_health();
    void enqueue_frame(VideoFrame&& frame);
    void update_title();
    void show_error(const std::wstring& message);

    HWND window_{};
    HWND mode_label_{};
    HMENU menu_bar_{};
    HMENU view_menu_{};
    HMENU processing_menu_{};
    HMENU nr_menu_{};
    HMENU nr_style_menu_{};
    HMENU nr_preset_menu_{};
    HMENU nr_intensity_menu_{};
    HMENU nr_latency_menu_{};
    HMENU device_menu_{};
    HMENU mode_menu_{};
    HMENU format_menu_{};
    HMENU resolution_menu_{};
    HMENU frame_rate_menu_{};
    HMENU image_menu_{};
    HMENU audio_menu_{};
    HWND mode_combo_{};
    WNDPROC original_combo_proc_{};
    std::unique_ptr<CaptureEngine> capture_;
    std::unique_ptr<AudioCapture> audio_capture_;
    std::vector<AudioCaptureDevice> audio_devices_;
    std::optional<size_t> active_audio_device_;
    std::vector<CaptureDevice> devices_;
    std::vector<CaptureMode> modes_;
    std::vector<std::wstring> formats_;
    std::vector<std::pair<uint32_t, uint32_t>> resolutions_;
    std::vector<std::pair<uint32_t, uint32_t>> frame_rates_;
    std::optional<size_t> active_device_;
    std::optional<size_t> active_mode_;
    D3D11Renderer renderer_;
    std::unique_ptr<IFrameProcessor> processor_;
    std::mutex frame_mutex_;
    std::optional<VideoFrame> pending_frame_;
    std::wstring status_ = L"Starting";
    std::atomic_bool flip_vertical_{false};
    uint64_t displayed_frames_{};
    bool fullscreen_{};
    bool always_on_top_{};
    bool auto_size_to_resolution_{};
    WINDOWPLACEMENT windowed_placement_{sizeof(WINDOWPLACEMENT)};
    std::wstring saved_device_name_;
    std::wstring saved_format_;
    std::wstring saved_audio_name_;
    uint32_t saved_width_{};
    uint32_t saved_height_{};
    uint32_t saved_fps_numerator_{};
    uint32_t saved_fps_denominator_{1};
    bool saved_flip_{};
    bool has_saved_flip_{};
    bool restoring_settings_{true};
    bool motion_analysis_enabled_{true};
    bool history_overlay_enabled_{};
    bool nr_enabled_{};
    bool nr_temporal_enabled_{true};
    uint32_t nr_style_{1};
    uint32_t nr_preset_{3};
    uint32_t nr_intensity_percent_{100};
    uint32_t nr_wait_ms_{2};
    PROCESS_INFORMATION worker_process_{};
    HANDLE temporal_mapping_{};
    WorkerTemporalState* temporal_state_{};
    uint64_t last_worker_restart_ms_{};
};
