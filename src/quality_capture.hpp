#pragma once
#include "common.hpp"
#include <d3d11.h>
#include <fstream>
#include <vector>

constexpr UINT quality_width = 320, quality_height = 180;
inline std::vector<uint8_t> quality_proxy(const uint8_t* bgra, UINT width, UINT height, size_t pitch) {
    std::vector<uint8_t> rgb(quality_width * quality_height * 3);
    for (UINT y = 0; y < quality_height; ++y) for (UINT x = 0; x < quality_width; ++x) {
        const UINT sx = static_cast<UINT>((static_cast<uint64_t>(x) * 2 + 1) * width / (quality_width * 2));
        const UINT sy = static_cast<UINT>((static_cast<uint64_t>(y) * 2 + 1) * height / (quality_height * 2));
        const uint8_t* p = bgra + sy * pitch + sx * 4;
        const size_t i = (static_cast<size_t>(y) * quality_width + x) * 3;
        rgb[i] = p[2]; rgb[i+1] = p[1]; rgb[i+2] = p[0];
    }
    return rgb;
}
class QualityCapture {
public:
    void initialize(ID3D11Device* device, D3D11_TEXTURE2D_DESC desc) {
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        throw_if_failed(device->CreateTexture2D(&desc, nullptr, &staging_), "Create quality readback");
    }
    void write(ID3D11DeviceContext* context, ID3D11Texture2D* texture, const uint8_t* source,
               UINT width, UINT height, std::ofstream& inputs, std::ofstream& outputs) {
        auto proxy = quality_proxy(source, width, height, static_cast<size_t>(width) * 4);
        inputs.write(reinterpret_cast<const char*>(proxy.data()), proxy.size());
        context->CopyResource(staging_.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        throw_if_failed(context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read quality output");
        try { proxy = quality_proxy(static_cast<const uint8_t*>(mapped.pData), width, height, mapped.RowPitch); }
        catch (...) { context->Unmap(staging_.Get(), 0); throw; }
        context->Unmap(staging_.Get(), 0);
        outputs.write(reinterpret_cast<const char*>(proxy.data()), proxy.size());
    }
private:
    ComPtr<ID3D11Texture2D> staging_;
};
