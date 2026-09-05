#pragma once
#include <d3d11.h>
#include <cstdint>
#include <d3d11_4.h>
#include <wrl/client.h>

// Keyed-mutex ownership alone is not a completion fence for a queued copy.
// Keep ownership until GPU work using the shared surface has completed.
class SharedCopyCompletion {
    Microsoft::WRL::ComPtr<ID3D11Query> query_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
    HANDLE event_{};
    UINT64 value_{};
    bool event_attempted_{}, prefer_event_{};
public:
    explicit SharedCopyCompletion(bool prefer_event=true):prefer_event_(prefer_event){}
    ~SharedCopyCompletion(){if(event_) CloseHandle(event_);}
    SharedCopyCompletion(const SharedCopyCompletion&)=delete;
    SharedCopyCompletion& operator=(const SharedCopyCompletion&)=delete;
    bool uses_event() const {return event_!=nullptr;}
    HRESULT wait(ID3D11DeviceContext* context) {
        if(prefer_event_ && !event_attempted_) {
            event_attempted_=true;
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            Microsoft::WRL::ComPtr<ID3D11Device5> device5;
            context->GetDevice(&device);
            if(SUCCEEDED(device.As(&device5)) && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4_))) &&
               SUCCEEDED(device5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_))))
                event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        }
        if(event_) {
            const UINT64 value=++value_;
            HRESULT hr=context4_->Signal(fence_.Get(),value);
            if(FAILED(hr)) return hr;
            context->Flush();
            const UINT64 completed=fence_->GetCompletedValue();
            if(completed==UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
            if(completed>=value) return S_OK;
            hr=fence_->SetEventOnCompletion(value,event_);
            if(FAILED(hr)) return hr;
            const DWORD result=WaitForSingleObject(event_,1000);
            if(result!=WAIT_OBJECT_0) return HRESULT_FROM_WIN32(result==WAIT_TIMEOUT?WAIT_TIMEOUT:GetLastError());
            const auto after=fence_->GetCompletedValue();
            if(after==UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
            return after>=value ? S_OK : E_FAIL;
        }
        if (!query_) {
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};
            const HRESULT hr=device->CreateQuery(&desc,&query_);
            if (FAILED(hr)) return hr;
        }
        context->End(query_.Get());
        context->Flush();
        const ULONGLONG deadline=GetTickCount64()+1000;
        for (;;) {
            BOOL done=FALSE;
            const HRESULT hr=context->GetData(query_.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (FAILED(hr) || (hr==S_OK && done)) return hr;
            if (GetTickCount64()>=deadline) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            SwitchToThread();
        }
    }
};
