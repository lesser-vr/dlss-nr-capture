#include "common.hpp"
#include "worker_protocol.hpp"
#include "shared_copy_completion.hpp"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <iostream>
#include <vector>

struct Child {
    PROCESS_INFORMATION info{};
    ~Child() { if (info.hProcess) { TerminateProcess(info.hProcess,0); WaitForSingleObject(info.hProcess,5000); CloseHandle(info.hProcess); CloseHandle(info.hThread); } }
};
int wmain(int argc,wchar_t** argv) {
    const bool failure_test=argc==4 && wcscmp(argv[2],L"--failure")==0;
    const bool warp=failure_test || (argc==3 && wcscmp(argv[2],L"--warp")==0);
    if (argc!=2 && !warp) return 2;
    try {
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr,warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) {
            if(warp) throw std::runtime_error("Required WARP device unavailable");
            std::cout<<"SKIP: hardware shared device unavailable\n"; return 77;
        }
        ComPtr<IDXGIDevice> dxgi; throw_if_failed(device.As(&dxgi),"DXGI device");
        ComPtr<IDXGIAdapter> adapter; throw_if_failed(dxgi->GetAdapter(&adapter),"adapter");
        DXGI_ADAPTER_DESC gpu{}; adapter->GetDesc(&gpu);
        std::wcout<<L"Pipeline adapter: "<<gpu.Description<<L" (WARP requested="<<warp<<L")\n";
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=64; desc.Height=32; desc.ArraySize=desc.MipLevels=1; desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtr<ID3D11Texture2D> input,output,readback;
        ComPtr<IDXGIKeyedMutex> input_mutex,output_mutex;
        HANDLE input_handle{},output_handle{};
        auto shared=[&](ComPtr<ID3D11Texture2D>& texture,ComPtr<IDXGIKeyedMutex>& mutex,HANDLE& handle) {
            throw_if_failed(device->CreateTexture2D(&desc,nullptr,&texture),"shared texture");
            throw_if_failed(texture.As(&mutex),"keyed mutex");
            ComPtr<IDXGIResource1> resource; throw_if_failed(texture.As(&resource),"resource");
            throw_if_failed(resource->CreateSharedHandle(&security,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle),"handle");
        };
        shared(input,input_mutex,input_handle); shared(output,output_mutex,output_handle);
        desc.MiscFlags=desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        throw_if_failed(device->CreateTexture2D(&desc,nullptr,&readback),"readback");
        HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,&security,PAGE_READWRITE,0,sizeof(WorkerTemporalState),nullptr);
        HANDLE ready=CreateEventW(&security,TRUE,FALSE,nullptr);
        auto* state=static_cast<WorkerTemporalState*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(WorkerTemporalState)));
        if (!state || !ready) throw std::runtime_error("test IPC allocation");
        *state=WorkerTemporalState{}; // NR off: built-in passthrough, no private DLL.
        if(failure_test){state->nr_enabled=1;state->nr_passes=2;}
        auto number=[](HANDLE h){return std::to_wstring(reinterpret_cast<uintptr_t>(h));};
        std::wstring command=L"\""+std::wstring(argv[1])+L"\" "+std::to_wstring(GetCurrentProcessId())+L" "+
            number(input_handle)+L" "+number(output_handle)+L" "+number(mapping)+L" "+number(ready);
        if(warp) command+=L" --warp-test";
        if(failure_test) command+=L" \""+std::wstring(argv[3])+L"\"";
        STARTUPINFOW startup{sizeof(startup)}; Child child;
        SharedCopyCompletion completion;
        if (!CreateProcessW(argv[1],command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child.info))
            throw std::runtime_error("start test worker");
        auto submit=[&](uint64_t sequence,bool valid=true) {
            if (input_mutex->AcquireSync(0,5000)!=S_OK) throw std::runtime_error("input blocked by held output");
            std::vector<uint32_t> pixels(64*32,0xff000000u|static_cast<uint32_t>(sequence));
            context->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),64*4,0);
            state->payload={}; state->payload.frame_sequence=sequence;
            state->payload.flags=valid?temporal_valid:0;
            MemoryBarrier(); throw_if_failed(completion.wait(context.Get()),"input complete"); input_mutex->ReleaseSync(1);
        };
        auto consume=[&]() {
            if (output_mutex->AcquireSync(1,5000)!=S_OK) throw std::runtime_error("output timeout");
            const auto sequence=state->output_frame_sequence;
            context->CopyResource(readback.Get(),output.Get());
            throw_if_failed(completion.wait(context.Get()),"output complete");
            output_mutex->ReleaseSync(0);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            throw_if_failed(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"read output");
            bool matches=true;
            const uint32_t first=*static_cast<const uint32_t*>(mapped.pData);
            for (UINT y=0;y<32;++y) for (UINT x=0;x<64;++x)
                matches &= reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch)[x]==(0xff000000u|sequence);
            context->Unmap(readback.Get(),0);
            if (!matches) {
                std::cerr<<"expected sequence="<<sequence<<" first pixel="<<std::hex<<first<<std::dec<<'\n';
                throw std::runtime_error("pixel and metadata frame mismatch");
            }
            return sequence;
        };
        if(failure_test) {
            for(uint64_t sequence=1;sequence<=3;++sequence) submit(sequence);
            submit(4);
            if(consume()!=4 || state->worker_adapter_state!=1 || state->worker_adapter_error!=5 ||
                std::wstring(state->worker_adapter_error_message)!=L"Injected two-pass adapter failure" || state->nr_passes!=2)
                throw std::runtime_error("Two-pass failure was not reported with original pixel fallback");
            std::cout<<"Two-pass payload, three failures, explicit error and original pixel fallback passed\n";
            return 0;
        }
        submit(1);
        if (WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0) throw std::runtime_error("worker not ready");
        // Do not consume output: worker must continue accepting input and drop bounded results.
        for (uint64_t sequence=2;sequence<=40;++sequence) submit(sequence);
        if (input_mutex->AcquireSync(0,5000)!=S_OK) throw std::runtime_error("input not released");
        input_mutex->ReleaseSync(0);
        const auto deadline=GetTickCount64()+5000;
        while (InterlockedCompareExchange64(&state->worker_processed_frames,0,0)<40 && GetTickCount64()<deadline) Sleep(1);
        if (InterlockedCompareExchange64(&state->worker_processed_frames,0,0)!=40 ||
            InterlockedCompareExchange64(&state->worker_dropped_outputs,0,0)<38)
            throw std::runtime_error("worker stalled or output queue grew");
        if (consume()!=1) throw std::runtime_error("held output overwritten");
        for (uint64_t sequence=41;sequence<=80;++sequence) { submit(sequence); if (consume()!=sequence) throw std::runtime_error("stale output"); }
        submit(81,false);
        if (output_mutex->AcquireSync(1,100)==S_OK) throw std::runtime_error("invalid frame published");
        std::cout<<"Worker pipeline: 80 paired frames, held output, bounded drops, recovery, invalid input passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
