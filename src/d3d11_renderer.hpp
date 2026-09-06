#pragma once
#include "shared_copy_completion.hpp"

#include "common.hpp"
#include "frame.hpp"
#include "nr_speed_policy.hpp"
#include "overlay_style.hpp"
#include "blackout_probe.hpp"
#include "gpu_memory_policy.hpp"

#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_6.h>
#include <string>
#include <utility>
#include <array>

class D3D11Renderer {
public:
    ~D3D11Renderer();
    void initialize(HWND window);
    void reset();
    bool presented() const {return presented_;}
    void set_comparison(bool enabled);
    void set_comparison_peek(bool value) { comparison_peek_=value; }
    void set_comparison_swap(bool value) { comparison_swap_=value; }
    void set_comparison_split(float value) { comparison_split_=value; }
    float comparison_split() const { return comparison_split_; }
    bool comparison_held() const {return comparison_hold_;}
    unsigned comparison_diagnostics() const {
        return (comparison_?1u:0u)|(comparison_peek_?2u:0u)|(comparison_swap_?4u:0u)|
            (comparison_hold_?8u:0u)|(comparison_pair_?16u:0u)|(correction_active_?32u:0u)|
            ((notification_message_==L"NR OFF - TURN ON NR WITH F10" && GetTickCount64()-nr_notification_started_ms_<1500)?64u:0u)|
            (static_cast<unsigned>(comparison_zoom_)<<8);
    }
    void set_comparison_hold(bool value) {comparison_hold_=value && comparison_ && comparison_pair_ && correction_active_;if(!comparison_hold_)comparison_zoom_=1;}
    void cycle_comparison_zoom() {if(comparison_hold_)comparison_zoom_=comparison_zoom_==1.0f?2.0f:comparison_zoom_==2.0f?4.0f:1.0f;}
    ID3D11Device* device() const noexcept { return device_.Get(); }
    void resize(uint32_t width, uint32_t height);
    void render(const VideoFrame& frame);
    void set_temporal_state(WorkerTemporalState* value) noexcept { temporal_state_ = value; }
    void clear();
    std::wstring take_diagnostic() { return std::exchange(diagnostic_, {}); }
    void redraw_idle();
    void set_capture_interrupted(bool value) noexcept { capture_interrupted_ = value;if(value)set_comparison_hold(false); }
    void show_nr_toggle(bool enabled);
    void show_notification(const std::wstring& message);
    void set_performance_text(const std::wstring& text) { performance_text_ = text; }
    uint64_t worker_average_processing_us() const noexcept { return speed_monitor_.average_us(); }
    void configure_shared_output(uint32_t width, uint32_t height,
                                 uint32_t fps_numerator, uint32_t fps_denominator);
    const std::wstring& shared_texture_name() const noexcept { return shared_texture_name_; }
    HANDLE shared_input_handle() const noexcept { return shared_input_handle_; }
    HANDLE shared_output_handle() const noexcept { return shared_output_handle_; }
    const std::wstring& worker_event_name() const noexcept { return worker_event_name_; }
    HANDLE worker_event_handle() const noexcept { return worker_event_; }
    bool worker_connected() const noexcept;
    uint64_t worker_output_frames() const noexcept { return worker_output_frames_; }
    uint64_t worker_enhanced_frames() const noexcept { return worker_enhanced_frames_; }
    uint64_t worker_fallback_frames() const noexcept { return worker_fallback_frames_; }
    bool worker_correction_active() const noexcept { return correction_active_; }
    bool worker_preparing() const noexcept {
        return correction_enabled_ && speed_monitor_.preparing();
    }
    bool worker_correction_too_slow() const noexcept {
        return correction_enabled_ && correction_available_ && speed_monitor_.slow();
    }
    bool worker_memory_pressure() const noexcept {
        if (!temporal_state_) return false;
        const auto stamp = InterlockedCompareExchange64(&temporal_state_->gpu_memory_sample_ms, 0, 0);
        const auto usage = InterlockedCompareExchange64(&temporal_state_->gpu_memory_usage, 0, 0);
        const auto budget = InterlockedCompareExchange64(&temporal_state_->gpu_memory_budget, 0, 0);
        return stamp == InterlockedCompareExchange64(&temporal_state_->gpu_memory_sample_ms, 0, 0) &&
            gpu_memory_pressure(usage, budget, stamp, GetTickCount64());
    }
    void set_worker_wait_ms(uint32_t value) noexcept { worker_wait_ms_ = value; }
    void set_worker_correction_enabled(bool value) noexcept { correction_enabled_ = value; }
    void set_worker_processing_time_us(uint64_t value) noexcept { worker_processing_time_us_ = value; }

private:
    struct CompareFrame { ComPtr<ID3D11Texture2D> texture; uint64_t sequence{}; bool valid{}; };
    std::array<CompareFrame,4> compare_frames_;
    size_t compare_next_{};
    ComPtr<ID3D11Texture2D> compare_original_;
    ComPtr<ID3D11ShaderResourceView> compare_original_view_;
    ComPtr<ID3D11Buffer> compare_constants_;
    bool comparison_{}, comparison_peek_{}, comparison_swap_{}, comparison_pair_{};
    float comparison_split_{0.5f};
    bool comparison_hold_{};
    float comparison_zoom_{1};
    void reset_comparison_frames();
    void remember_comparison_frame(uint64_t sequence);
    void match_comparison_frame(uint64_t sequence);
    void draw_comparison_overlay();
    bool presented_{};
    WorkerTemporalState* temporal_state_{};
    void present(UINT interval, UINT flags);
    BlackoutProbe blackout_probe_;
    std::wstring diagnostic_;
    int last_blackout_state_{-1};
    HRESULT last_present_result_{S_OK};
    uint64_t last_probe_log_ms_{};
    void ensure_frame_texture(uint32_t width, uint32_t height);
    void create_back_buffer();
    void initialize_correction_pipeline();
    void initialize_overlay_pipeline();
    void draw_status_overlay();
    void draw_performance_overlay();
    void draw_status_banner(const std::wstring& message, OverlayMessageStyle style,
                            float opacity = 1.0f);
    void render_with_correction();

    HWND window_{};
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    SharedCopyCompletion shared_copy_completion_;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<ID3D11RenderTargetView> render_target_;
    ComPtr<ID3D11Texture2D> frame_texture_;
    ComPtr<ID3D11ShaderResourceView> frame_view_;
    uint32_t frame_width_{};
    uint32_t frame_height_{};
    ComPtr<ID3D11Texture2D> shared_input_texture_;
    ComPtr<IDXGIKeyedMutex> shared_input_mutex_;
    ComPtr<ID3D11Texture2D> shared_output_texture_;
    ComPtr<IDXGIKeyedMutex> shared_output_mutex_;
    ComPtr<ID3D11Texture2D> correction_texture_;
    ComPtr<ID3D11ShaderResourceView> correction_view_;
    ComPtr<ID3D11VertexShader> correction_vertex_shader_;
    ComPtr<ID3D11PixelShader> correction_pixel_shader_;
    ComPtr<ID2D1Factory1> d2d_factory_;
    ComPtr<ID2D1Device> d2d_device_;
    ComPtr<ID2D1DeviceContext> d2d_context_;
    ComPtr<ID2D1Bitmap1> d2d_target_;
    ComPtr<ID2D1SolidColorBrush> warning_background_brush_;
    ComPtr<ID2D1SolidColorBrush> warning_text_brush_;
    ComPtr<IDWriteFactory> dwrite_factory_;
    ComPtr<IDWriteTextFormat> warning_text_format_;
    ComPtr<IDWriteTextFormat> performance_text_format_;
    ComPtr<IDWriteTextFormat> comparison_text_format_;
    std::wstring performance_text_;
    std::wstring shared_texture_name_;
    std::wstring worker_event_name_;
    HANDLE worker_event_{};
    HANDLE shared_input_handle_{};
    HANDLE shared_output_handle_{};
    uint32_t shared_width_{};
    uint32_t shared_height_{};
    uint32_t shared_generation_{};
    uint64_t worker_output_frames_{};
    uint64_t worker_enhanced_frames_{};
    uint64_t worker_fallback_frames_{};
    uint64_t correction_updated_tick_ms_{};
    uint64_t worker_processing_time_us_{};
    NrSpeedMonitor speed_monitor_{};
    uint32_t worker_wait_ms_{2};
    bool correction_enabled_{};
    bool correction_available_{};
    bool correction_active_{};
    bool capture_interrupted_{};
    uint64_t nr_notification_started_ms_{};
    bool nr_notification_visible_{};
    std::wstring notification_message_;
};
