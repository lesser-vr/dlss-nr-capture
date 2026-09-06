#include "d3d11_renderer.hpp"
#include "nr_notification.hpp"

#include <algorithm>
#include <d3dcompiler.h>
#include <new>

void D3D11Renderer::reset() {
    // Caller has stopped capture and the worker and released all borrowed views.
    this->~D3D11Renderer();
    new(this) D3D11Renderer();
}

D3D11Renderer::~D3D11Renderer()
{
    if (worker_event_) CloseHandle(worker_event_);
    if (shared_input_handle_) CloseHandle(shared_input_handle_);
    if (shared_output_handle_) CloseHandle(shared_output_handle_);
}

void D3D11Renderer::configure_shared_output(uint32_t width, uint32_t height,
                                          uint32_t fps_numerator, uint32_t fps_denominator)
{
    reset_comparison_frames();
    shared_input_mutex_.Reset();
    shared_input_texture_.Reset();
    shared_output_mutex_.Reset();
    shared_output_texture_.Reset();
    correction_view_.Reset();
    correction_texture_.Reset();
    if (worker_event_) { CloseHandle(worker_event_); worker_event_ = nullptr; }
    if (shared_input_handle_) { CloseHandle(shared_input_handle_); shared_input_handle_ = nullptr; }
    if (shared_output_handle_) { CloseHandle(shared_output_handle_); shared_output_handle_ = nullptr; }
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
    auto create_shared = [&](ComPtr<ID3D11Texture2D>& texture, ComPtr<IDXGIKeyedMutex>& mutex,
                             HANDLE& handle, const char* operation) {
        throw_if_failed(device_->CreateTexture2D(&description, nullptr, &texture), operation);
        ComPtr<IDXGIResource1> resource;
        throw_if_failed(texture.As(&resource), "Query shared resource");
        throw_if_failed(resource->CreateSharedHandle(&security,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle),
            "Create shared frame handle");
        throw_if_failed(texture.As(&mutex), "Query shared keyed mutex");
    };
    create_shared(shared_input_texture_, shared_input_mutex_, shared_input_handle_,
                  "Create shared input texture");
    create_shared(shared_output_texture_, shared_output_mutex_, shared_output_handle_,
                  "Create shared output texture");
    description.MiscFlags = 0;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    throw_if_failed(device_->CreateTexture2D(&description, nullptr, &correction_texture_),
                    "Create correction cache texture");
    throw_if_failed(device_->CreateShaderResourceView(correction_texture_.Get(), nullptr, &correction_view_),
                    "Create correction cache view");
    shared_width_ = width; shared_height_ = height;
    worker_output_frames_ = worker_enhanced_frames_ = worker_fallback_frames_ = 0;
    correction_updated_tick_ms_ = 0;
    worker_processing_time_us_ = 0;
    speed_monitor_.reset(fps_numerator, fps_denominator);
    correction_available_ = correction_active_ = false;
}

bool D3D11Renderer::worker_connected() const noexcept
{
    return worker_event_ && WaitForSingleObject(worker_event_, 0) == WAIT_OBJECT_0;
}

void D3D11Renderer::initialize(HWND window)
{
    window_ = window;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL level{};
    throw_if_failed(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                      nullptr, 0, D3D11_SDK_VERSION, &device_, &level,
                                      &context_), "D3D11CreateDevice");
    ComPtr<ID3D10Multithread> protection;
    if (SUCCEEDED(device_.As(&protection))) protection->SetMultithreadProtected(TRUE);
    initialize_correction_pipeline();
    initialize_overlay_pipeline();

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

void D3D11Renderer::initialize_correction_pipeline()
{
    static constexpr char vertex_source[] =
        "float4 main(uint id : SV_VertexID) : SV_POSITION {"
        "float2 p = float2((id << 1) & 2, id & 2);"
        "return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);}";
    static constexpr char pixel_source[] =
        "Texture2D<float4> base_frame : register(t0);"
        "Texture2D<float4> correction : register(t1);"
        "cbuffer Comparison : register(b0) { float split; float mode; float swapped; float zoom; };"
        "float4 main(float4 position : SV_POSITION) : SV_TARGET {"
        "int2 p = int2(position.xy);"
        "uint w,h; correction.GetDimensions(w,h); p=int2((float2(p)-float2(w,h)*0.5)/max(zoom,1.0)+float2(w,h)*0.5);"
        "bool original = mode > 1.5 || (mode > 0.5 && ((position.x < split) != (swapped > 0.5)));"
        "if(original) return float4(base_frame.Load(int3(p,0)).rgb,1.0);"
        "return float4(correction.Load(int3(p, 0)).rgb, 1.0);}";
    ComPtr<ID3DBlob> vertex_blob, pixel_blob, errors;
    throw_if_failed(D3DCompile(vertex_source, sizeof(vertex_source) - 1, nullptr, nullptr, nullptr,
                               "main", "vs_5_0", 0, 0, &vertex_blob, &errors),
                    "Compile correction vertex shader");
    errors.Reset();
    throw_if_failed(D3DCompile(pixel_source, sizeof(pixel_source) - 1, nullptr, nullptr, nullptr,
                               "main", "ps_5_0", 0, 0, &pixel_blob, &errors),
                    "Compile correction pixel shader");
    throw_if_failed(device_->CreateVertexShader(vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
                                                 nullptr, &correction_vertex_shader_),
                    "Create correction vertex shader");
    throw_if_failed(device_->CreatePixelShader(pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(),
                                                nullptr, &correction_pixel_shader_),
                    "Create correction pixel shader");
    D3D11_BUFFER_DESC constants{}; constants.ByteWidth=16;
    constants.Usage=D3D11_USAGE_DEFAULT; constants.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    throw_if_failed(device_->CreateBuffer(&constants,nullptr,&compare_constants_),"Create comparison constants");
}

void D3D11Renderer::initialize_overlay_pipeline()
{
    D2D1_FACTORY_OPTIONS options{};
#ifdef _DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    throw_if_failed(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), &options,
        reinterpret_cast<void**>(d2d_factory_.GetAddressOf())), "Create D2D factory");
    ComPtr<IDXGIDevice> dxgi_device;
    throw_if_failed(device_.As(&dxgi_device), "Query overlay DXGI device");
    throw_if_failed(d2d_factory_->CreateDevice(dxgi_device.Get(), &d2d_device_),
                    "Create D2D device");
    throw_if_failed(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                      &d2d_context_),
                    "Create D2D context");
    throw_if_failed(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf())),
                    "Create DirectWrite factory");
    throw_if_failed(dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        24.0f, L"en-us", &warning_text_format_), "Create warning text format");
    warning_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    warning_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    throw_if_failed(dwrite_factory_->CreateTextFormat(L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        16.0f, L"en-us", &performance_text_format_), "Create performance text format");
    throw_if_failed(dwrite_factory_->CreateTextFormat(L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        18.0f, L"en-us", &comparison_text_format_), "Create comparison text format");
    comparison_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    comparison_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    throw_if_failed(d2d_context_->CreateSolidColorBrush(
        D2D1::ColorF(0xA81919, 0.88f), &warning_background_brush_),
                    "Create warning background brush");
    throw_if_failed(d2d_context_->CreateSolidColorBrush(
        D2D1::ColorF(D2D1::ColorF::White), &warning_text_brush_),
                    "Create warning text brush");
}

void D3D11Renderer::draw_status_overlay()
{
    draw_performance_overlay();
    if (capture_interrupted_) {
        draw_status_banner(L"CAPTURE INTERRUPTED - WAITING FOR INPUT", OverlayMessageStyle::error);
        return;
    }
    const uint64_t now = GetTickCount64();
    const float opacity = nr_notification_visible_
        ? nr_notification_opacity(now - nr_notification_started_ms_) : 0.0f;
    const bool notification = opacity > 0.0f;
    const bool preparing = worker_preparing();
    if (!notification) nr_notification_visible_ = false;
    if (notification)
        draw_status_banner(notification_message_,
                           OverlayMessageStyle::information, opacity);
    else if (preparing)
        draw_status_banner(L"NR PREPARING", OverlayMessageStyle::information);
    else if (worker_correction_too_slow())
        draw_status_banner(worker_memory_pressure()
            ? L"POSSIBLE VRAM PRESSURE\nSHOWING ORIGINAL VIDEO"
            : L"NR TOO SLOW - SHOWING ORIGINAL VIDEO", OverlayMessageStyle::error);
}

void D3D11Renderer::draw_status_banner(const std::wstring& message,
                                      OverlayMessageStyle style, float opacity)
{
    if (!d2d_context_ || !d2d_target_) return;
    const auto palette = overlay_palette(style);
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    warning_background_brush_->SetColor(
        D2D1::ColorF(palette.background_rgb, palette.background_opacity * opacity));
    warning_text_brush_->SetColor(D2D1::ColorF(palette.text_rgb, opacity));
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    d2d_context_->BeginDraw();
    const D2D1_SIZE_F size = d2d_context_->GetSize();
    const float width = std::min(520.0f, std::max(280.0f, size.width - 32.0f));
    const D2D1_RECT_F banner = D2D1::RectF((size.width - width) * 0.5f, 20.0f,
                                           (size.width + width) * 0.5f, 88.0f);
    d2d_context_->FillRoundedRectangle(D2D1::RoundedRect(banner, 10.0f, 10.0f),
                                       warning_background_brush_.Get());
    d2d_context_->DrawTextW(message.c_str(), static_cast<UINT32>(message.size()), warning_text_format_.Get(), banner,
                            warning_text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    const HRESULT result = d2d_context_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET)
        d2d_target_.Reset();
    else
        throw_if_failed(result, "Draw status overlay");
}

void D3D11Renderer::draw_performance_overlay()
{
    if (performance_text_.empty() || !d2d_context_ || !d2d_target_) return;
    const auto size = d2d_context_->GetSize();
    // Keep clear of the top status banner, even in a small window.
    if (size.width < 240 || size.height < 240) return;
    const auto palette = overlay_palette(OverlayMessageStyle::information);
    warning_background_brush_->SetColor(D2D1::ColorF(palette.background_rgb, palette.background_opacity));
    warning_text_brush_->SetColor(D2D1::ColorF(palette.text_rgb));
    const auto box = D2D1::RectF(12, size.height - 110, std::min(size.width - 12, 552.0f), size.height - 12);
    const auto text_box = D2D1::RectF(box.left + 8, box.top + 8, box.right - 8, box.bottom - 8);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    d2d_context_->BeginDraw();
    d2d_context_->FillRoundedRectangle(D2D1::RoundedRect(box, 6, 6), warning_background_brush_.Get());
    d2d_context_->DrawTextW(performance_text_.c_str(), static_cast<UINT32>(performance_text_.size()),
        performance_text_format_.Get(), text_box, warning_text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    const HRESULT result = d2d_context_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) d2d_target_.Reset();
    else throw_if_failed(result, "Draw performance overlay");
}

void D3D11Renderer::show_nr_toggle(bool enabled)
{
    show_notification(enabled ? L"NR ON" : L"NR OFF");
}

void D3D11Renderer::show_notification(const std::wstring& message)
{
    notification_message_ = message;
    nr_notification_started_ms_ = GetTickCount64();
    nr_notification_visible_ = true;
}

void D3D11Renderer::render_with_correction()
{
    const float constants[]={comparison_split_*frame_width_,comparison_peek_?2.0f:comparison_?1.0f:0.0f,comparison_swap_?1.0f:0.0f,comparison_zoom_};
    context_->UpdateSubresource(compare_constants_.Get(),0,nullptr,constants,0,0);
    ID3D11Buffer* buffer=compare_constants_.Get(); context_->PSSetConstantBuffers(0,1,&buffer);
    ID3D11RenderTargetView* target = render_target_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(frame_width_),
                            static_cast<float>(frame_height_), 0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(correction_vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(correction_pixel_shader_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* views[] = {comparison_?compare_original_view_.Get():frame_view_.Get(), correction_view_.Get()};
    context_->PSSetShaderResources(0, 2, views);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* empty[] = {nullptr, nullptr};
    context_->PSSetShaderResources(0, 2, empty);
}

void D3D11Renderer::reset_comparison_frames() {
    comparison_hold_=false;comparison_zoom_=1;
    for(auto& frame:compare_frames_) frame=CompareFrame{};
    compare_original_view_.Reset();compare_original_.Reset();comparison_pair_=false;compare_next_=0;
}
void D3D11Renderer::set_comparison(bool enabled) {
    if(comparison_!=enabled) reset_comparison_frames();
    comparison_=enabled;comparison_peek_=false;
}
void D3D11Renderer::remember_comparison_frame(uint64_t sequence) {
    const uint64_t bytes=uint64_t(frame_width_)*frame_height_*4;
    // Keep history plus the matched-original texture within 128 MiB.
    const uint64_t slots=bytes ? (128ull*1024*1024)/bytes : 0;
    if(slots<2) return;
    const size_t capacity=static_cast<size_t>(std::min<uint64_t>(compare_frames_.size(),slots-1));
    compare_next_%=capacity;
    auto& slot=compare_frames_[compare_next_];
    if(!slot.texture) {
        D3D11_TEXTURE2D_DESC desc{};frame_texture_->GetDesc(&desc);
        throw_if_failed(device_->CreateTexture2D(&desc,nullptr,&slot.texture),"Create comparison history");
    }
    context_->CopyResource(slot.texture.Get(),frame_texture_.Get());
    slot.sequence=sequence;slot.valid=true;compare_next_=(compare_next_+1)%capacity;
}
void D3D11Renderer::match_comparison_frame(uint64_t sequence) {
    comparison_pair_=false;
    for(const auto& slot:compare_frames_) if(slot.valid && slot.sequence==sequence) {
        if(!compare_original_) {
            D3D11_TEXTURE2D_DESC desc{};slot.texture->GetDesc(&desc);
            throw_if_failed(device_->CreateTexture2D(&desc,nullptr,&compare_original_),"Create paired original");
            throw_if_failed(device_->CreateShaderResourceView(compare_original_.Get(),nullptr,&compare_original_view_),"Create paired original view");
        }
        context_->CopyResource(compare_original_.Get(),slot.texture.Get());comparison_pair_=true;break;
    }
}
void D3D11Renderer::draw_comparison_overlay() {
    if((!comparison_ && !comparison_peek_) || !d2d_context_ || !d2d_target_) return;
    const bool ready=correction_active_ && comparison_pair_ && !capture_interrupted_;
    const wchar_t* nr=ready?(comparison_hold_?L"NR HELD (F8 RESUME)":L"NR ACTIVE"):!correction_enabled_?L"NR OFF - ORIGINAL":worker_preparing()?L"PREPARING":L"ORIGINAL FALLBACK";
    const auto size=d2d_context_->GetSize();
    const auto palette=overlay_palette(OverlayMessageStyle::information);
    warning_background_brush_->SetColor(D2D1::ColorF(palette.background_rgb,1));
    warning_text_brush_->SetColor(D2D1::ColorF(palette.text_rgb));
    context_->OMSetRenderTargets(0,nullptr,nullptr);d2d_context_->BeginDraw();
    auto label=[&](const wchar_t* text,float left,float right) {
        const auto box=D2D1::RectF(left,96,right,132);
        d2d_context_->FillRectangle(box,warning_background_brush_.Get());
        d2d_context_->DrawTextW(text,static_cast<UINT32>(wcslen(text)),comparison_text_format_.Get(),box,warning_text_brush_.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const float width=std::min(240.0f,size.width*0.45f);
    label(comparison_peek_?L"ORIGINAL (TAB)":comparison_swap_?nr:L"ORIGINAL",8,8+width);
    if(!comparison_peek_) {
        label(comparison_swap_?L"ORIGINAL":nr,size.width-width-8,size.width-8);
        const float x=size.width*comparison_split_;
        d2d_context_->DrawLine(D2D1::Point2F(x,0),D2D1::Point2F(x,size.height),warning_text_brush_.Get(),2);
    }
    const HRESULT result=d2d_context_->EndDraw();
    if(result==D2DERR_RECREATE_TARGET)d2d_target_.Reset();else throw_if_failed(result,"Draw comparison labels");
}

void D3D11Renderer::create_back_buffer()
{
    ComPtr<ID3D11Texture2D> buffer;
    throw_if_failed(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Get swap-chain buffer");
    throw_if_failed(device_->CreateRenderTargetView(buffer.Get(), nullptr, &render_target_),
                    "CreateRenderTargetView");
    ComPtr<IDXGISurface> surface;
    throw_if_failed(buffer.As(&surface), "Query overlay back buffer surface");
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    throw_if_failed(d2d_context_->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
                                                               &d2d_target_),
                    "Create overlay target");
    d2d_context_->SetTarget(d2d_target_.Get());
}

void D3D11Renderer::resize(uint32_t width, uint32_t height)
{
    if (!swap_chain_ || width == 0 || height == 0)
        return;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    d2d_context_->SetTarget(nullptr);
    d2d_target_.Reset();
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
    reset_comparison_frames();
    frame_view_.Reset();
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    throw_if_failed(device_->CreateTexture2D(&description, nullptr, &frame_texture_),
                    "Create frame texture");
    throw_if_failed(device_->CreateShaderResourceView(frame_texture_.Get(), nullptr, &frame_view_),
                    "Create frame view");
    frame_width_ = width;
    frame_height_ = height;
}

void D3D11Renderer::render(const VideoFrame& frame)
{
    presented_=false;
    if (!swap_chain_ || frame.bgra.empty())
        return;
    ensure_frame_texture(frame.width, frame.height);
    if(comparison_hold_) {
        render_with_correction();draw_status_overlay();draw_comparison_overlay();
        present(0,DXGI_PRESENT_DO_NOT_WAIT);
        // Held pixels do not correspond to this incoming frame's timestamp.
        presented_=false;return;
    }
    if (const auto sample = blackout_probe_.poll(context_.Get())) {
        const int state = (sample->source_dark ? 1 : 0) | (sample->output_dark ? 2 : 0) |
                          (sample->nr_active ? 4 : 0);
        const auto tick = GetTickCount64();
        if (state != last_blackout_state_ || tick - last_probe_log_ms_ >= 60000) {
            diagnostic_ += L"Video probe: source_dark=" + std::to_wstring(sample->source_dark) +
                L" output_dark=" + std::to_wstring(sample->output_dark) +
                L" nr_active=" + std::to_wstring(sample->nr_active) + L"; ";
            last_blackout_state_ = state; last_probe_log_ms_ = tick;
        }
    }
    if (frame.gpu) context_->CopyResource(frame_texture_.Get(), frame.gpu->texture.Get());
    else context_->UpdateSubresource(frame_texture_.Get(), 0, nullptr, frame.bgra.data(), frame.width * 4, 0);

    if (shared_input_texture_ && shared_width_ == frame.width && shared_height_ == frame.height &&
        shared_input_mutex_->AcquireSync(0, 0) == S_OK) {
        context_->CopyResource(shared_input_texture_.Get(), frame_texture_.Get());
        if(comparison_ && correction_enabled_) remember_comparison_frame(frame.temporal.frame_sequence);
        if (temporal_state_) {
            temporal_state_->payload = frame.temporal;
            MemoryBarrier();
        }
        // Complete shared-resource access before granting the other process ownership.
        throw_if_failed(shared_copy_completion_.wait(context_.Get()), "Shared input copy completion");
        shared_input_mutex_->ReleaseSync(1);
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
    // Cache only newly completed correction data. A stale correction expires automatically.
    if (shared_output_texture_ &&
        shared_output_mutex_->AcquireSync(1, worker_wait_ms_) == S_OK) {
        if (correction_enabled_) {
            context_->CopyResource(correction_texture_.Get(), shared_output_texture_.Get());
            if(comparison_) {
                comparison_pair_=false;
                if(temporal_state_) match_comparison_frame(temporal_state_->output_frame_sequence);
            }
            const uint64_t update_tick = GetTickCount64();
            speed_monitor_.observe(update_tick, temporal_state_ ? temporal_state_->output_processing_us : worker_processing_time_us_);
            correction_updated_tick_ms_ = temporal_state_ ? temporal_state_->output_completed_ms : update_tick;
            correction_available_ = true;
            ++worker_output_frames_;
            throw_if_failed(shared_copy_completion_.wait(context_.Get()), "Shared output copy completion");
        }
        shared_output_mutex_->ReleaseSync(0);
    }
    const uint64_t now = GetTickCount64();
    correction_active_ = correction_enabled_ && correction_available_ &&
        speed_monitor_.fast() &&
        now >= correction_updated_tick_ms_ &&
        now - correction_updated_tick_ms_ <= 100;
    if(comparison_ && (!comparison_pair_ || !temporal_state_ ||
        InterlockedCompareExchange(&temporal_state_->worker_adapter_state,0,0)!=2)) correction_active_=false;
    if (correction_active_) {
        render_with_correction();
        ++worker_enhanced_frames_;
    } else {
        context_->CopyResource(back_buffer.Get(), frame_texture_.Get());
        ++worker_fallback_frames_;
    }
    // Sample the rendered video before drawing messages, without blocking for a readback.
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    blackout_probe_.submit(device_.Get(), context_.Get(), back_buffer.Get(), frame.bgra.data(),
        frame.bgra.size(), frame.width, frame.height, correction_active_, now,
        frame.gpu ? 96 : frame.width, frame.gpu ? frame.analysis_height : frame.height);
    draw_status_overlay();
    draw_comparison_overlay();
    present(0, DXGI_PRESENT_DO_NOT_WAIT);
}

void D3D11Renderer::clear()
{
    if (!render_target_)
        return;
    constexpr float color[] = {0.015f, 0.018f, 0.024f, 1.0f};
    context_->ClearRenderTargetView(render_target_.Get(), color);
    present(1, 0);
}

void D3D11Renderer::redraw_idle()
{
    if (!swap_chain_ || !render_target_) return;
    // Repaint cached original pixels only: never resubmit input or count this as a video frame.
    correction_active_ = false;
    ComPtr<ID3D11Texture2D> buffer;
    throw_if_failed(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Get idle back buffer");
    D3D11_TEXTURE2D_DESC desc{};
    buffer->GetDesc(&desc);
    if (frame_texture_ && desc.Width == frame_width_ && desc.Height == frame_height_) {
        context_->CopyResource(buffer.Get(), frame_texture_.Get());
    } else {
        constexpr float color[] = {0.015f, 0.018f, 0.024f, 1.0f};
        context_->ClearRenderTargetView(render_target_.Get(), color);
    }
    draw_status_overlay();
    present(0, DXGI_PRESENT_DO_NOT_WAIT);
}

void D3D11Renderer::present(UINT interval, UINT flags)
{
    const HRESULT hr = swap_chain_->Present(interval, flags);
    presented_=hr==S_OK;
    // A busy nonblocking present is a dropped presentation, not device removal.
    if (hr != DXGI_ERROR_WAS_STILL_DRAWING && hr != last_present_result_) {
        wchar_t text[160]{};
        swprintf_s(text, L"Present result=0x%08X device_removed_reason=0x%08X; ",
                   static_cast<unsigned>(hr), static_cast<unsigned>(device_->GetDeviceRemovedReason()));
        diagnostic_ += text;
        last_present_result_ = hr;
    }
}
