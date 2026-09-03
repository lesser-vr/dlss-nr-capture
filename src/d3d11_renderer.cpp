#include "d3d11_renderer.hpp"

#include <algorithm>

D3D11Renderer::~D3D11Renderer()
{
    if (worker_event_) CloseHandle(worker_event_);
    if (shared_texture_handle_) CloseHandle(shared_texture_handle_);
}

void D3D11Renderer::configure_shared_output(uint32_t width, uint32_t height)
{
    shared_mutex_.Reset();
    shared_texture_.Reset();
    if (worker_event_) { CloseHandle(worker_event_); worker_event_ = nullptr; }
    if (shared_texture_handle_) { CloseHandle(shared_texture_handle_); shared_texture_handle_ = nullptr; }
    ++shared_generation_;
    const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(shared_generation_);
    shared_texture_name_ = L"Local\\DlssNrCaptureTexture-" + suffix;
    worker_event_name_ = L"Local\\DlssNrCaptureWorker-" + suffix;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    worker_event_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!worker_event_) throw std::runtime_error("Create worker-ready event failed");
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width; description.Height = height; description.MipLevels = 1; description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    throw_if_failed(device_->CreateTexture2D(&description, nullptr, &shared_texture_), "Create shared frame texture");
    ComPtr<IDXGIResource1> resource; throw_if_failed(shared_texture_.As(&resource), "Query shared resource");
    throw_if_failed(resource->CreateSharedHandle(&security, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                 nullptr, &shared_texture_handle_), "Create shared frame handle");
    throw_if_failed(shared_texture_.As(&shared_mutex_), "Query shared keyed mutex");
    shared_width_ = width; shared_height_ = height;
    worker_output_frames_ = worker_fallback_frames_ = 0;
}

bool D3D11Renderer::worker_connected() const noexcept
{
    return worker_event_ && WaitForSingleObject(worker_event_, 0) == WAIT_OBJECT_0;
}

void D3D11Renderer::initialize(HWND window)
{
    window_ = window;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL level{};
    throw_if_failed(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                      nullptr, 0, D3D11_SDK_VERSION, &device_, &level,
                                      &context_), "D3D11CreateDevice");

    ComPtr<IDXGIDevice> dxgi_device;
    throw_if_failed(device_.As(&dxgi_device), "Query IDXGIDevice");
    ComPtr<IDXGIAdapter> adapter;
    throw_if_failed(dxgi_device->GetAdapter(&adapter), "Get DXGI adapter");
    ComPtr<IDXGIFactory2> factory;
    throw_if_failed(adapter->GetParent(IID_PPV_ARGS(&factory)), "Get DXGI factory");

    RECT area{};
    GetClientRect(window_, &area);
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = std::max<LONG>(1, area.right - area.left);
    description.Height = std::max<LONG>(1, area.bottom - area.top);
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    throw_if_failed(factory->CreateSwapChainForHwnd(device_.Get(), window_, &description,
                                                     nullptr, nullptr, &swap_chain_),
                    "CreateSwapChainForHwnd");
    factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    create_back_buffer();
}

void D3D11Renderer::create_back_buffer()
{
    ComPtr<ID3D11Texture2D> buffer;
    throw_if_failed(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Get swap-chain buffer");
    throw_if_failed(device_->CreateRenderTargetView(buffer.Get(), nullptr, &render_target_),
                    "CreateRenderTargetView");
}

void D3D11Renderer::resize(uint32_t width, uint32_t height)
{
    if (!swap_chain_ || width == 0 || height == 0)
        return;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    render_target_.Reset();
    throw_if_failed(swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0),
                    "ResizeBuffers");
    create_back_buffer();
}

void D3D11Renderer::ensure_frame_texture(uint32_t width, uint32_t height)
{
    if (frame_texture_ && frame_width_ == width && frame_height_ == height)
        return;
    frame_texture_.Reset();
    frame_view_.Reset();
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    throw_if_failed(device_->CreateTexture2D(&description, nullptr, &frame_texture_),
                    "Create frame texture");
    throw_if_failed(device_->CreateShaderResourceView(frame_texture_.Get(), nullptr, &frame_view_),
                    "Create frame view");
    frame_width_ = width;
    frame_height_ = height;
}

void D3D11Renderer::render(const VideoFrame& frame)
{
    if (!swap_chain_ || frame.bgra.empty())
        return;
    ensure_frame_texture(frame.width, frame.height);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    throw_if_failed(context_->Map(frame_texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                    "Map frame texture");
    const size_t source_pitch = static_cast<size_t>(frame.width) * 4;
    for (uint32_t row = 0; row < frame.height; ++row) {
        memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
               frame.bgra.data() + static_cast<size_t>(row) * source_pitch, source_pitch);
    }
    context_->Unmap(frame_texture_.Get(), 0);

    bool published_to_worker = false;
    if (shared_texture_ && shared_width_ == frame.width && shared_height_ == frame.height &&
        SUCCEEDED(shared_mutex_->AcquireSync(0, 0))) {
        context_->CopyResource(shared_texture_.Get(), frame_texture_.Get());
        shared_mutex_->ReleaseSync(1);
        published_to_worker = true;
    }

    ComPtr<ID3D11Texture2D> back_buffer;
    throw_if_failed(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)), "Get back buffer");
    D3D11_TEXTURE2D_DESC back_desc{};
    back_buffer->GetDesc(&back_desc);

    if (back_desc.Width != frame.width || back_desc.Height != frame.height) {
        back_buffer.Reset();
        resize(frame.width, frame.height);
        throw_if_failed(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)),
                        "Get resized back buffer");
    }
    // Wait only for the configured latency budget, then present the original frame.
    if (published_to_worker && SUCCEEDED(shared_mutex_->AcquireSync(0, worker_wait_ms_))) {
        context_->CopyResource(back_buffer.Get(), shared_texture_.Get());
        shared_mutex_->ReleaseSync(0);
        ++worker_output_frames_;
    } else {
        context_->CopyResource(back_buffer.Get(), frame_texture_.Get());
        ++worker_fallback_frames_;
    }
    swap_chain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
}

void D3D11Renderer::clear()
{
    if (!render_target_)
        return;
    constexpr float color[] = {0.015f, 0.018f, 0.024f, 1.0f};
    context_->ClearRenderTargetView(render_target_.Get(), color);
    swap_chain_->Present(1, 0);
}
