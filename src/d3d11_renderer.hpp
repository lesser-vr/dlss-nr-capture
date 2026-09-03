#pragma once

#include "common.hpp"
#include "frame.hpp"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <string>

class D3D11Renderer {
public:
    ~D3D11Renderer();
    void initialize(HWND window);
    void resize(uint32_t width, uint32_t height);
    void render(const VideoFrame& frame);
    void clear();
    void configure_shared_output(uint32_t width, uint32_t height);
    const std::wstring& shared_texture_name() const noexcept { return shared_texture_name_; }
    HANDLE shared_texture_handle() const noexcept { return shared_texture_handle_; }
    const std::wstring& worker_event_name() const noexcept { return worker_event_name_; }
    HANDLE worker_event_handle() const noexcept { return worker_event_; }
    bool worker_connected() const noexcept;
    uint64_t worker_output_frames() const noexcept { return worker_output_frames_; }
    uint64_t worker_fallback_frames() const noexcept { return worker_fallback_frames_; }
    void set_worker_wait_ms(uint32_t value) noexcept { worker_wait_ms_ = value; }

private:
    void ensure_frame_texture(uint32_t width, uint32_t height);
    void create_back_buffer();

    HWND window_{};
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<ID3D11RenderTargetView> render_target_;
    ComPtr<ID3D11Texture2D> frame_texture_;
    ComPtr<ID3D11ShaderResourceView> frame_view_;
    uint32_t frame_width_{};
    uint32_t frame_height_{};
    ComPtr<ID3D11Texture2D> shared_texture_;
    ComPtr<IDXGIKeyedMutex> shared_mutex_;
    std::wstring shared_texture_name_;
    std::wstring worker_event_name_;
    HANDLE worker_event_{};
    HANDLE shared_texture_handle_{};
    uint32_t shared_width_{};
    uint32_t shared_height_{};
    uint32_t shared_generation_{};
    uint64_t worker_output_frames_{};
    uint64_t worker_fallback_frames_{};
    uint32_t worker_wait_ms_{2};
};
