#pragma once
#include "gpu_capture.hpp"
#include <cmath>
inline void check_capture_colors(ID3D11Device* device,ID3D11DeviceContext* context){
 GpuCaptureConverter converter;unsigned cases=0;
 for(bool bt601:{false,true})for(bool full:{false,true})for(bool flip:{false,true})for(auto fmt:{DXGI_FORMAT_NV12,DXGI_FORMAT_P010}){
  CaptureColor color;color.bt601=bt601;color.full=full;
  D3D11_TEXTURE2D_DESC d{};d.Width=192;d.Height=108;d.ArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=fmt;d.BindFlags=D3D11_BIND_RENDER_TARGET;
  const UINT unit=fmt==DXGI_FORMAT_P010?2:1,pitch=192*unit;
  std::vector<uint8_t> pixels(pitch*162);
  auto write=[&](size_t i,unsigned v){if(unit==1)pixels[i]=uint8_t(v);else{uint16_t word=uint16_t(v<<8);memcpy(pixels.data()+i,&word,2);}};
  for(UINT y=0;y<108;y++)for(UINT x=0;x<192;x++){write(y*pitch+x*unit,y<54?81:145);if(!(y%2)&&!(x%2)){write(108*pitch+y/2*pitch+x*unit,90);write(108*pitch+y/2*pitch+(x+1)*unit,200);}}
  D3D11_SUBRESOURCE_DATA data{pixels.data(),pitch,0};ComPtr<ID3D11Texture2D> input,readback;
  throw_if_failed(device->CreateTexture2D(&d,&data,&input),"color input");d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=0;
  throw_if_failed(device->CreateTexture2D(&d,nullptr,&readback),"color readback");
  std::shared_ptr<GpuCaptureSurface> out;std::vector<uint8_t> analysis;UINT height;
  if(!converter.convert(device,input.Get(),0,out,analysis,height,color,flip))throw std::runtime_error("color/flip conversion unsupported");
  context->CopyResource(readback.Get(),out->texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};throw_if_failed(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"color map");
  unsigned max_error=0;
  for(UINT y:{12u,90u}){
   const double luma=((flip?107-y:y)<54?81:145),l=full?luma:(luma-16)*255/219;
   const double u=-38*(full?1.0:255.0/224),v=72*(full?1.0:255.0/224);
   const double values[]={l+(bt601?1.772:1.8556)*u,l-(bt601?.344136:.187324)*u-(bt601?.714136:.468124)*v,l+(bt601?1.402:1.5748)*v};
   const auto* p=static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch+96*4;
   for(int c=0;c<3;c++)max_error=std::max(max_error,unsigned(std::abs(int(p[c])-int(std::clamp(std::lround(values[c]),0l,255l)))));
   uint8_t cpu[4];color.yuv(uint8_t(luma),90,200,cpu);
   for(int c=0;c<3;c++)max_error=std::max(max_error,unsigned(std::abs(int(p[c])-int(cpu[c]))));
  }
  context->Unmap(readback.Get(),0);
  if(max_error>3)throw std::runtime_error("601/709 range or vertical flip mismatch");++cases;
 }
 std::cout<<"Color/flip: "<<cases<<" NV12/P010 601/709 full/limited cases passed\n";
 for(bool full:{false,true}){
  CaptureColor color;color.rgb=true;color.full=full;
  D3D11_TEXTURE2D_DESC d{};d.Width=192;d.Height=108;d.ArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
  std::vector<uint32_t> data(192*108,full?0xffff1300u:0xffeb2010u);
  D3D11_SUBRESOURCE_DATA initial{data.data(),192*4,0};ComPtr<ID3D11Texture2D> input;
  throw_if_failed(device->CreateTexture2D(&d,&initial,&input),"RGB range source");
  std::shared_ptr<GpuCaptureSurface> out;std::vector<uint8_t> analysis;UINT height;
  if(!converter.convert(device,input.Get(),0,out,analysis,height,color))throw std::runtime_error("RGB range conversion");
  for(size_t i=0;i<analysis.size();i+=4)if(analysis[i]>2 || std::abs(int(analysis[i+1])-19)>2 || analysis[i+2]<253)throw std::runtime_error("RGB nominal range mismatch");
 }
}
