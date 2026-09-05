#pragma once
#include "common.hpp"
#include <d3d11.h>
#include <optional>

// Sparse diagnostics only, not a visual-quality verdict. No image is saved.
struct BlackoutSample { bool source_dark{}, output_dark{}, nr_active{}; };
class BlackoutProbe {
    ComPtr<ID3D11Texture2D> staging_;
    bool pending_{}, source_dark_{}, nr_active_{};
    uint64_t submitted_{};
public:
    std::optional<BlackoutSample> poll(ID3D11DeviceContext* context) {
        if (!pending_) return {};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context->Map(staging_.Get(), 0, D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return {};
        pending_ = false;
        if (FAILED(hr)) return {};
        bool dark = true;
        const auto* pixels = static_cast<const uint8_t*>(mapped.pData);
        for (UINT i = 0; i < 9; ++i)
            for (UINT c = 0; c < 3; ++c) dark &= pixels[i * 4 + c] <= 8;
        context->Unmap(staging_.Get(), 0);
        return BlackoutSample{source_dark_, dark, nr_active_};
    }
    void submit(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* output,
                const uint8_t* source, size_t bytes, UINT width, UINT height,
                bool nr_active, uint64_t now, UINT source_width = 0, UINT source_height = 0) {
        if (!source_width) source_width = width;
        if (!source_height) source_height = height;
        if (pending_ || (submitted_ && now - submitted_ < 1000) || !source ||
            !width || !height || bytes < static_cast<size_t>(source_width) * source_height * 4) return;
        if (!staging_) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = 9; desc.Height = 1; desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging_))) return;
        }
        D3D11_TEXTURE2D_DESC desc{}; output->GetDesc(&desc);
        if (desc.Width != width || desc.Height != height || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return;
        source_dark_ = true; nr_active_ = nr_active;
        for (UINT y = 0; y < 3; ++y) for (UINT x = 0; x < 3; ++x) {
            const UINT sx = width * (2 * x + 1) / 6, sy = height * (2 * y + 1) / 6;
            const UINT px = source_width * (2 * x + 1) / 6, py = source_height * (2 * y + 1) / 6;
            const auto* pixel = source + (static_cast<size_t>(py) * source_width + px) * 4;
            for (UINT c = 0; c < 3; ++c) source_dark_ &= pixel[c] <= 8;
            const D3D11_BOX box{sx, sy, 0, sx + 1, sy + 1, 1};
            context->CopySubresourceRegion(staging_.Get(), 0, y * 3 + x, 0, 0, output, 0, &box);
        }
        pending_ = true; submitted_ = now;
    }
};
