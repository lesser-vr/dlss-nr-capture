#pragma once
#include <d3d11.h>
#include <wrl/client.h>

// Keyed-mutex ownership alone is not a completion fence for a queued copy.
// Keep ownership until GPU work using the shared surface has completed.
class SharedCopyCompletion {
    Microsoft::WRL::ComPtr<ID3D11Query> query_;
public:
    HRESULT wait(ID3D11DeviceContext* context) {
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
