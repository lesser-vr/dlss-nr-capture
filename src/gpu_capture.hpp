#pragma once
#include "common.hpp"
#include <d3d11.h>
#include <d3d10.h>
#include <algorithm>
#include <cstring>
#include <array>
#include <memory>
#include <vector>

struct GpuCaptureSurface {
    ComPtr<ID3D11Texture2D> texture;
};

// Three bounded owned surfaces: queued/UI frames keep their slot alive.
// VideoProcessor converts NV12/P010/BGRA to BGRA and a small analysis image.
class GpuCaptureConverter {
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> video_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11Texture2D> small_, readback_;
    std::array<std::shared_ptr<GpuCaptureSurface>,3> slots_;
    UINT width_{}, height_{}, small_height_{};
    DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
public:
    bool convert(ID3D11Device* device, ID3D11Texture2D* source, UINT subresource,
                 std::shared_ptr<GpuCaptureSurface>& surface, std::vector<uint8_t>& analysis,
                 UINT& analysis_height) {
        if (!device || !source) return false;
        ComPtr<ID3D10Multithread> protection;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&protection)))) return false;
        struct Guard {
            ID3D10Multithread* value;
            explicit Guard(ID3D10Multithread* p) : value(p) { value->Enter(); }
            ~Guard() { value->Leave(); }
        } guard(protection.Get());
        D3D11_TEXTURE2D_DESC input{}; source->GetDesc(&input);
        if (input.Format != DXGI_FORMAT_NV12 && input.Format != DXGI_FORMAT_P010 &&
            input.Format != DXGI_FORMAT_B8G8R8A8_UNORM && input.Format != DXGI_FORMAT_B8G8R8X8_UNORM &&
            input.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
        if (!input.Width || !input.Height || input.SampleDesc.Count != 1 ||
            input.MipLevels != 1 || subresource >= input.ArraySize) return false;
        ComPtr<ID3D11Device> owner; source->GetDevice(&owner);
        if (owner.Get() != device) return false;
        if (device_.Get() != device || width_ != input.Width || height_ != input.Height || format_ != input.Format) {
            *this = GpuCaptureConverter{};
            device_ = device; device->GetImmediateContext(&context_);
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&video_))) ||
                FAILED(context_.As(&video_context_))) return false;
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
            content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content.InputWidth = content.OutputWidth = input.Width;
            content.InputHeight = content.OutputHeight = input.Height;
            content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            if (FAILED(video_->CreateVideoProcessorEnumerator(&content,&enumerator_)) ||
                FAILED(video_->CreateVideoProcessor(enumerator_.Get(),0,&processor_))) return false;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = input.Width; desc.Height = input.Height;
            desc.ArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            for (auto& slot : slots_) {
                slot = std::make_shared<GpuCaptureSurface>();
                if (FAILED(device->CreateTexture2D(&desc,nullptr,&slot->texture))) return false;
            }
            desc.Width = 96; desc.Height = std::max(1u, (input.Height * 96 + input.Width / 2) / input.Width);
            small_height_ = desc.Height;
            if (FAILED(device->CreateTexture2D(&desc,nullptr,&small_))) return false;
            desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&desc,nullptr,&readback_))) return false;
            width_ = input.Width; height_ = input.Height; format_ = input.Format;
        }
        std::shared_ptr<GpuCaptureSurface>* available = nullptr;
        for (auto& slot : slots_) if (slot.use_count() == 1) { available = &slot; break; }
        if (!available) return false;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in{};
        in.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        in.Texture2D.ArraySlice = subresource;
        ComPtr<ID3D11VideoProcessorInputView> input_view;
        if (FAILED(video_->CreateVideoProcessorInputView(source,enumerator_.Get(),&in,&input_view))) return false;
        D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = input_view.Get();
        RECT src{0,0,static_cast<LONG>(width_),static_cast<LONG>(height_)};
        video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(),0,TRUE,&src);
        video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(),0,FALSE);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_color{};
        input_color.YCbCr_Matrix = 1; // BT.709, matching capture conversion.
        input_color.Nominal_Range = (format_ == DXGI_FORMAT_NV12 || format_ == DXGI_FORMAT_P010)
            ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_color{};
        output_color.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(),0,&input_color);
        video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(),&output_color);
        const auto blit = [&](ID3D11Texture2D* target, UINT w, UINT h) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out{}; out.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            ComPtr<ID3D11VideoProcessorOutputView> view;
            if (FAILED(video_->CreateVideoProcessorOutputView(target,enumerator_.Get(),&out,&view))) return false;
            RECT rect{0,0,static_cast<LONG>(w),static_cast<LONG>(h)};
            video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(),TRUE,&rect);
            video_context_->VideoProcessorSetStreamDestRect(processor_.Get(),0,TRUE,&rect);
            return SUCCEEDED(video_context_->VideoProcessorBlt(processor_.Get(),view.Get(),0,1,&stream));
        };
        if (!blit((*available)->texture.Get(),width_,height_) || !blit(small_.Get(),96,small_height_)) return false;
        context_->CopyResource(readback_.Get(),small_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(readback_.Get(),0,D3D11_MAP_READ,0,&mapped))) return false;
        analysis.resize(static_cast<size_t>(96)*small_height_*4);
        for (UINT y=0;y<small_height_;++y)
            memcpy(analysis.data()+static_cast<size_t>(y)*96*4,
                static_cast<const uint8_t*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch,96*4);
        context_->Unmap(readback_.Get(),0);
        surface = *available; analysis_height = small_height_; return true;
    }
};
