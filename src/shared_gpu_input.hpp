#pragma once
#include "common.hpp"
#include <d3d11_1.h>
#include <d3d12.h>

// Single-slot handoff. Caller must finish D3D12 work and return the resource to
// COMMON before the next copy or destruction. No CPU pixel mapping is involved.
class SharedGpuInput {
public:
    HRESULT create(ID3D12Device* device, UINT width, UINT height) {
        reset();
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = height;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource_));
    }
    HRESULT copy_from(ID3D12Device* device12, ID3D11Device* device11,
                      ID3D11DeviceContext* context, ID3D11Texture2D* source) {
        if (!resource_ || !source || !context || !device11) return E_INVALIDARG;
        if (FAILED(copy_failure_)) return copy_failure_;
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        const auto desc = resource_->GetDesc();
        if (source_desc.Width != desc.Width || source_desc.Height != desc.Height ||
            source_desc.Format != desc.Format || source_desc.SampleDesc.Count != 1 ||
            source_desc.ArraySize != 1 || source_desc.MipLevels != 1) return E_INVALIDARG;
        if (!texture_) {
            ComPtr<ID3D11Device1> device1;
            HRESULT hr = device11->QueryInterface(IID_PPV_ARGS(&device1));
            if (FAILED(hr)) return hr;
            HANDLE shared{};
            hr = device12->CreateSharedHandle(resource_.Get(), nullptr, GENERIC_ALL, nullptr, &shared);
            if (FAILED(hr)) return hr;
            hr = device1->OpenSharedResource1(shared, IID_PPV_ARGS(&texture_));
            CloseHandle(shared);
            if (FAILED(hr)) return hr;
            D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
            hr = device11->CreateQuery(&query, &copy_done_);
            if (FAILED(hr)) { texture_.Reset(); return hr; }
        }
        context->CopyResource(texture_.Get(), source);
        context->End(copy_done_.Get());
        context->Flush();
        const ULONGLONG deadline = GetTickCount64() + 1000;
        for (;;) {
            const HRESULT hr = context->GetData(copy_done_.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr != S_FALSE) { copy_failure_ = hr; return hr; }
            if (GetTickCount64() >= deadline) {
                copy_failure_ = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                return copy_failure_; // Do not reuse a slot with unknown ownership.
            }
            Sleep(0);
        }
    }
    ID3D12Resource* resource() const noexcept { return resource_.Get(); }
    void reset() noexcept { copy_done_.Reset(); texture_.Reset(); resource_.Reset(); copy_failure_ = S_OK; }
private:
    HRESULT copy_failure_{S_OK};
    ComPtr<ID3D12Resource> resource_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11Query> copy_done_;
};
