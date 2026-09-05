#pragma once
#include "gpu_capture.hpp"
#include <iostream>

// Compare SDR video conversion with the existing CPU BT.709 integer reference.
// Ignore a four-pixel strip at bar boundaries, where chroma resampling differs.
inline void check_capture_matrix(ID3D11Device* device, ID3D11DeviceContext* context) {
    const int bars[8][3]={{16,128,128},{235,128,128},{81,90,240},{145,54,34},
                          {41,240,110},{126,128,128},{170,166,16},{210,16,146}};
    GpuCaptureConverter converter;
    unsigned max_error=0, frames=0;
    for (const auto size : {std::pair{1920u,1080u},std::pair{2560u,1440u},
                            std::pair{3840u,2160u},std::pair{1280u,720u}}) {
        const auto [width,height]=size;
        for (auto format : {DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_NV12,DXGI_FORMAT_P010}) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width=width; desc.Height=height; desc.ArraySize=desc.MipLevels=1;
            desc.SampleDesc.Count=1; desc.Format=format; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> source,readback;
            throw_if_failed(device->CreateTexture2D(&desc,nullptr,&source),"matrix source");
            desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags=0;
            desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            throw_if_failed(device->CreateTexture2D(&desc,nullptr,&readback),"matrix readback");
            const bool rgb=format==DXGI_FORMAT_B8G8R8A8_UNORM;
            const UINT unit=format==DXGI_FORMAT_P010?2:1;
            const UINT pitch=width*(rgb?4:unit);
            std::vector<uint8_t> input(static_cast<size_t>(pitch)*height*(rgb?2:3)/2);
            std::shared_ptr<GpuCaptureSurface> output;
            std::vector<uint8_t> analysis; UINT analysis_height{};
            for (UINT frame=0;frame<3;++frame) {
                uint8_t expected[8][3]{};
                for (UINT bar=0;bar<8;++bar) {
                    const auto& yuv=bars[(bar+frame)%8];
                    const int c=std::max(0,yuv[0]-16),u=yuv[1]-128,v=yuv[2]-128;
                    const int channels[3]={(298*c+541*u+128)>>8,(298*c-55*u-136*v+128)>>8,(298*c+459*v+128)>>8};
                    for (UINT ch=0;ch<3;++ch) expected[bar][ch]=static_cast<uint8_t>(std::clamp(channels[ch],0,255));
                }
                auto write=[&](size_t index,int value) {
                    if(unit==1) input[index]=static_cast<uint8_t>(value);
                    else { const uint16_t word=static_cast<uint16_t>(value<<8); memcpy(input.data()+index,&word,2); }
                };
                for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x) {
                    const UINT bar=x/(width/8);
                    const auto& yuv=bars[(bar+frame)%8];
                    const size_t offset=static_cast<size_t>(y)*pitch+x*(rgb?4:unit);
                    if(rgb) { memcpy(input.data()+offset,expected[bar],3); input[offset+3]=255; }
                    else {
                        write(offset,yuv[0]);
                        if (!(x%2) && !(y%2)) {
                            const size_t uv=static_cast<size_t>(pitch)*height+static_cast<size_t>(y/2)*pitch+x*unit;
                            write(uv,yuv[1]); write(uv+unit,yuv[2]);
                        }
                    }
                }
                context->UpdateSubresource(source.Get(),0,nullptr,input.data(),pitch,0);
                if(!converter.convert(device,source.Get(),0,output,analysis,analysis_height)) throw std::runtime_error("matrix conversion failed");
                if(analysis_height!=54 || analysis.size()!=96*54*4) throw std::runtime_error("matrix analysis dimensions");
                context->CopyResource(readback.Get(),output->texture.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                throw_if_failed(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"matrix map");
                unsigned error=0;
                for(UINT y=0;y<height;++y) for(UINT x=4;x<width-4;++x) {
                    const UINT local=x%(width/8);
                    if(local<4 || local>=width/8-4) continue;
                    const auto* pixel=static_cast<const uint8_t*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch+x*4;
                    for(UINT ch=0;ch<3;++ch) error=std::max(error,static_cast<unsigned>(abs(static_cast<int>(pixel[ch])-expected[x/(width/8)][ch])));
                }
                context->Unmap(readback.Get(),0);
                max_error=std::max(max_error,error); ++frames;
                if(error>3) { std::cerr<<"size="<<width<<'x'<<height<<" format="<<format<<" frame="<<frame<<" max_error="<<error<<'\n'; throw std::runtime_error("matrix color/stale-frame mismatch"); }
            }
        }
    }
    std::cout<<"Capture matrix: "<<frames<<" changing frames, 1080p/1440p/4K/720p, BGRA/NV12/P010, max RGB error="<<max_error<<"/255\n";
}
