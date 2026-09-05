#pragma once
#include "common.hpp"
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3d12.h>

// Single-slot handoff. Caller must finish D3D12 work and return the resource to
// COMMON before the next copy or destruction. No CPU pixel mapping is involved.
class SharedGpuInput {
public:
    HRESULT create(ID3D12Device* device, UINT width, UINT height,
                   DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM) {
        reset();
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = height;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (format == DXGI_FORMAT_R16G16_TYPELESS)
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource_));
    }
    HRESULT copy_from(ID3D12Device* device12, ID3D11Device* device11,
                      ID3D11DeviceContext* context, ID3D11Texture2D* source,
                      ID3D12CommandQueue* consumer_queue = nullptr) {
        if (!resource_ || !source || !context || !device11 || !device12) return E_INVALIDARG;
        if (FAILED(copy_failure_)) return copy_failure_;
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        const auto desc = resource_->GetDesc();
        const bool compatible_format = source_desc.Format == desc.Format ||
            (source_desc.Format == DXGI_FORMAT_R16G16_SINT && desc.Format == DXGI_FORMAT_R16G16_TYPELESS);
        if (source_desc.Width != desc.Width || source_desc.Height != desc.Height ||
            !compatible_format || source_desc.SampleDesc.Count != 1 ||
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
        if (consumer_queue && !fence_attempted_) {
            fence_attempted_ = true;
            ComPtr<ID3D11Device5> device5;
            if (SUCCEEDED(device11->QueryInterface(IID_PPV_ARGS(&device5))) &&
                SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4_))) &&
                SUCCEEDED(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12_)))) {
                HANDLE handle{};
                if (SUCCEEDED(device12->CreateSharedHandle(fence12_.Get(), nullptr, GENERIC_ALL, nullptr, &handle))) {
                    device5->OpenSharedFence(handle, IID_PPV_ARGS(&fence11_));
                    CloseHandle(handle);
                }
            }
            if (!fence11_) { fence12_.Reset(); context4_.Reset(); }
        }
        context->CopyResource(texture_.Get(), source);
        if (consumer_queue && fence11_) {
            // Queue ordering replaces CPU polling. The caller still waits for
            // all D3D12 work before the next copy/reset (single-slot ownership).
            const UINT64 value = ++fence_value_;
            HRESULT hr = context4_->Signal(fence11_.Get(), value);
            context->Flush();
            if (SUCCEEDED(hr)) hr = consumer_queue->Wait(fence12_.Get(), value);
            if (FAILED(hr)) copy_failure_ = hr;
            return hr;
        }
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
    bool gpu_fence_available() const noexcept { return fence11_ != nullptr; }
    void reset() noexcept {
        fence11_.Reset(); fence12_.Reset(); context4_.Reset();
        fence_attempted_ = false; fence_value_ = 0;
        copy_done_.Reset(); texture_.Reset(); resource_.Reset(); copy_failure_ = S_OK;
    }
private:
    HRESULT copy_failure_{S_OK};
    ComPtr<ID3D12Resource> resource_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11Query> copy_done_;
    ComPtr<ID3D11DeviceContext4> context4_;
    ComPtr<ID3D11Fence> fence11_;
    ComPtr<ID3D12Fence> fence12_;
    bool fence_attempted_{};
    UINT64 fence_value_{};
};
