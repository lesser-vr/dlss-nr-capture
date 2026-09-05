#include "gpu_capture.hpp"
#include "frame_processor.hpp"
#include "gpu_capture_matrix.hpp"
#include <mfapi.h>
#include <mfobjects.h>
#include <iostream>

int main() {
    try {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const HRESULT created = D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
        if (FAILED(created)) { std::cout << "SKIP: hardware video device unavailable\n"; return 77; }
        // D3D_DRIVER_TYPE_HARDWARE can succeed on hosted/virtual adapters
        // without a video processor. Check that independent capability first;
        // converter failures after this gate remain test failures.
        ComPtr<ID3D11VideoDevice> video;
        ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
        ComPtr<ID3D11VideoProcessor> capability_processor;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth=content.OutputWidth=192;
        content.InputHeight=content.OutputHeight=108;
        content.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT video_hr=device.As(&video);
        if(SUCCEEDED(video_hr)) video_hr=video->CreateVideoProcessorEnumerator(&content,&enumerator);
        if(SUCCEEDED(video_hr)) video_hr=video->CreateVideoProcessor(enumerator.Get(),0,&capability_processor);
        if(FAILED(video_hr)) {
            std::cout<<"SKIP: video processor unavailable, HRESULT="<<std::hex<<static_cast<unsigned>(video_hr)<<'\n';
            return 77;
        }
        ComPtr<ID3D10Multithread> protection;
        throw_if_failed(device.As(&protection),"multithread protection");
        protection->SetMultithreadProtected(TRUE);
        throw_if_failed(MFStartup(MF_VERSION),"MFStartup");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=192; desc.Height=108; desc.ArraySize=2; desc.MipLevels=1;
        desc.SampleDesc.Count=1; desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> source;
        throw_if_failed(device->CreateTexture2D(&desc,nullptr,&source),"source");
        std::vector<uint32_t> pixels(192*108,0xff2040c0);
        context->UpdateSubresource(source.Get(),1,nullptr,pixels.data(),192*4,0);
        ComPtr<IMFMediaBuffer> buffer;
        throw_if_failed(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),source.Get(),1,FALSE,&buffer),"DXGI buffer");
        ComPtr<IMFDXGIBuffer> dxgi; throw_if_failed(buffer.As(&dxgi),"DXGI interface");
        UINT subresource{}; throw_if_failed(dxgi->GetSubresourceIndex(&subresource),"array slice");
        if (subresource!=1) throw std::runtime_error("sample slice lost");
        GpuCaptureConverter converter;
        std::array<std::shared_ptr<GpuCaptureSurface>,3> held;
        std::vector<uint8_t> analysis; UINT height{};
        for (auto& surface:held) {
            if (!converter.convert(device.Get(),source.Get(),subresource,surface,analysis,height))
                throw std::runtime_error("GPU capture conversion failed");
            if (height!=54 || analysis.size()!=96*54*4) throw std::runtime_error("analysis dimensions");
            for (size_t i=0;i<analysis.size();i+=4)
                for (int c=0;c<3;++c)
                    if (abs(static_cast<int>(analysis[i+c])-static_cast<int>((0xff2040c0u>>(c*8))&255))>2)
                        throw std::runtime_error("analysis color mismatch");
        }
        std::shared_ptr<GpuCaptureSurface> fourth;
        if (converter.convert(device.Get(),source.Get(),1,fourth,analysis,height))
            throw std::runtime_error("pool overwrote leased frame");
        if (held[0]->texture.Get()==held[1]->texture.Get()) throw std::runtime_error("aliased leases");
        desc.ArraySize=1; desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        throw_if_failed(device->CreateTexture2D(&desc,nullptr,&readback),"readback");
        context->CopyResource(readback.Get(),held[0]->texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        throw_if_failed(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"map");
        bool matches=true;
        for (UINT y=0;y<108;++y) for (UINT x=0;x<192;++x) for (UINT c=0;c<3;++c) {
            const auto value=static_cast<const uint8_t*>(mapped.pData)[y*mapped.RowPitch+x*4+c];
            matches &= abs(static_cast<int>(value)-static_cast<int>((0xff2040c0u>>(c*8))&255))<=2;
        }
        context->Unmap(readback.Get(),0);
        if (!matches) throw std::runtime_error("full-resolution color mismatch");
        held[0].reset();
        if (!converter.convert(device.Get(),source.Get(),1,fourth,analysis,height)) throw std::runtime_error("lease not reclaimed");
        VideoFrame frame; frame.width=192; frame.height=108; frame.gpu=fourth;
        frame.bgra=analysis; frame.analysis_height=height;
        auto processor=create_motion_analysis_processor();
        if (!processor->process(frame)) throw std::runtime_error("GPU analysis rejected");
        frame.sequence=1;
        if (!processor->process(frame) || processor->temporal_state().frame_sequence!=1)
            throw std::runtime_error("GPU metadata sequence lost");
        frame.bgra.clear();
        if (processor->process(frame)) throw std::runtime_error("short GPU analysis accepted");
        if (converter.convert(device.Get(),source.Get(),2,fourth,analysis,height)) throw std::runtime_error("invalid slice accepted");
        // SDR luma endpoints exercise video-range expansion for both planar formats.
        for (DXGI_FORMAT format : {DXGI_FORMAT_NV12, DXGI_FORMAT_P010}) {
            for (UINT luma : {16u, 235u}) {
                D3D11_TEXTURE2D_DESC planar{};
                planar.Width=192; planar.Height=108; planar.ArraySize=planar.MipLevels=1;
                planar.SampleDesc.Count=1; planar.Format=format;
                planar.BindFlags=D3D11_BIND_RENDER_TARGET;
                const UINT pitch=192*(format==DXGI_FORMAT_P010?2:1);
                std::vector<uint8_t> data(pitch*108*3/2);
                if (format==DXGI_FORMAT_NV12) {
                    std::fill(data.begin(),data.begin()+pitch*108,static_cast<uint8_t>(luma));
                    std::fill(data.begin()+pitch*108,data.end(),static_cast<uint8_t>(128));
                } else {
                    for (size_t i=0;i<data.size();i+=2) {
                        const uint16_t value=static_cast<uint16_t>((i<pitch*108?luma:128u)<<8);
                        memcpy(data.data()+i,&value,2);
                    }
                }
                D3D11_SUBRESOURCE_DATA initial{data.data(),pitch,0};
                ComPtr<ID3D11Texture2D> yuv;
                throw_if_failed(device->CreateTexture2D(&planar,&initial,&yuv),"planar source");
                if (!converter.convert(device.Get(),yuv.Get(),0,fourth,analysis,height)) throw std::runtime_error("planar conversion");
                for (size_t i=0;i<analysis.size();i+=4) for (int c=0;c<3;++c)
                    if (abs(static_cast<int>(analysis[i+c])-(luma==16?0:255))>2) {
                        std::cerr << "format=" << format << " luma=" << luma << " value=" << static_cast<int>(analysis[i+c]) << '\n';
                        throw std::runtime_error("planar range mismatch");
                    }
            }
        }
        check_capture_matrix(device.Get(),context.Get());
        MFShutdown();
        std::cout << "GPU capture: array slice, full/small color, 3 leases, reuse and analysis passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
