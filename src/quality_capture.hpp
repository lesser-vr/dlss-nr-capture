#pragma once
#include "common.hpp"
#include <d3d11.h>
#include <fstream>
#include <vector>

constexpr UINT quality_width = 320, quality_height = 180;
inline std::vector<uint8_t> quality_proxy(const uint8_t* bgra, UINT width, UINT height, size_t pitch,
    UINT output_width = quality_width, UINT output_height = quality_height,
    UINT region_x = 0, UINT region_y = 0, UINT region_width = 0, UINT region_height = 0) {
    if (!region_width) region_width = width;
    if (!region_height) region_height = height;
    if (!output_width || !output_height || region_x >= width || region_y >= height ||
        region_width > width - region_x || region_height > height - region_y)
        throw std::runtime_error("Invalid quality region");
    std::vector<uint8_t> rgb(static_cast<size_t>(output_width) * output_height * 3);
    for (UINT y = 0; y < output_height; ++y) for (UINT x = 0; x < output_width; ++x) {
        const UINT sx = region_x + static_cast<UINT>((static_cast<uint64_t>(x) * 2 + 1) * region_width / (output_width * 2));
        const UINT sy = region_y + static_cast<UINT>((static_cast<uint64_t>(y) * 2 + 1) * region_height / (output_height * 2));
        const uint8_t* p = bgra + sy * pitch + sx * 4;
        const size_t i = (static_cast<size_t>(y) * output_width + x) * 3;
        rgb[i] = p[2]; rgb[i+1] = p[1]; rgb[i+2] = p[0];
    }
    return rgb;
}
class QualityCapture {
public:
    void initialize(ID3D11Device* device, D3D11_TEXTURE2D_DESC desc,
                    UINT ow, UINT oh, UINT x, UINT y, UINT w, UINT h) {
        ow_ = ow; oh_ = oh; x_ = x; y_ = y; w_ = w; h_ = h;
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        throw_if_failed(device->CreateTexture2D(&desc, nullptr, &staging_), "Create quality readback");
    }
    void write(ID3D11DeviceContext* context, ID3D11Texture2D* texture, const uint8_t* source,
               UINT width, UINT height, std::ofstream& inputs, std::ofstream& outputs) {
        auto proxy = quality_proxy(source, width, height, static_cast<size_t>(width) * 4, ow_, oh_, x_, y_, w_, h_);
        inputs.write(reinterpret_cast<const char*>(proxy.data()), proxy.size());
        context->CopyResource(staging_.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        throw_if_failed(context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read quality output");
        try { proxy = quality_proxy(static_cast<const uint8_t*>(mapped.pData), width, height, mapped.RowPitch, ow_, oh_, x_, y_, w_, h_); }
        catch (...) { context->Unmap(staging_.Get(), 0); throw; }
        context->Unmap(staging_.Get(), 0);
        outputs.write(reinterpret_cast<const char*>(proxy.data()), proxy.size());
    }
private:
    UINT ow_{}, oh_{}, x_{}, y_{}, w_{}, h_{};
    ComPtr<ID3D11Texture2D> staging_;
};
