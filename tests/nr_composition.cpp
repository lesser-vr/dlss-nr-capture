#include "nr_composition.hpp"
#include <cmath>
#include <iostream>
#include <vector>

int main(){try{
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
 throw_if_failed(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Create WARP");
 D3D11_TEXTURE2D_DESC desc{};desc.Width=64;desc.Height=48;desc.MipLevels=desc.ArraySize=1;
 desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
 ComPtr<ID3D11Texture2D> input;throw_if_failed(device->CreateTexture2D(&desc,nullptr,&input),"Input");
 auto read=[&](ID3D11Texture2D* texture){
  D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=d.MiscFlags=0;
  ComPtr<ID3D11Texture2D> staging;throw_if_failed(device->CreateTexture2D(&d,nullptr,&staging),"Staging");context->CopyResource(staging.Get(),texture);
  D3D11_MAPPED_SUBRESOURCE m{};throw_if_failed(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m),"Read result");
  std::vector<unsigned char> bytes(size_t(d.Width)*d.Height*4);
  for(UINT y=0;y<d.Height;y++)memcpy(bytes.data()+size_t(y)*d.Width*4,static_cast<unsigned char*>(m.pData)+size_t(y)*m.RowPitch,d.Width*4);
  context->Unmap(staging.Get(),0);return bytes;
 };
 int cases=0;
 for(UINT scale:{100u,75u,50u}){
  // Identity residual must preserve a detailed original, not its enlargement.
  std::vector<unsigned char> pixels(64*48*4);
  for(size_t i=0;i<pixels.size();i+=4){pixels[i]=static_cast<unsigned char>((i/4*37)%256);pixels[i+1]=static_cast<unsigned char>((i/4*13)%256);pixels[i+2]=90;pixels[i+3]=255;}
  context->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),64*4,0);
  NrComposition composition;const auto reduced_desc=composition.initialize(device.Get(),desc,scale,0,false);
  auto* low=composition.prepare(context.Get(),input.Get());
  const auto reduced=read(low);
  // Independent CPU overlap integration for every reduced pixel/channel.
  for(UINT y=0;y<reduced_desc.Height;y++)for(UINT x=0;x<reduced_desc.Width;x++)for(int ch=0;ch<3;ch++){
   const double x0=double(x)*64/reduced_desc.Width,x1=double(x+1)*64/reduced_desc.Width,y0=double(y)*48/reduced_desc.Height,y1=double(y+1)*48/reduced_desc.Height;
   double sum=0;
   for(int sy=int(std::floor(y0));sy<int(std::ceil(y1));sy++)for(int sx=int(std::floor(x0));sx<int(std::ceil(x1));sx++)
    sum+=pixels[(sy*64+sx)*4+ch]*(std::min(x1,double(sx+1))-std::max(x0,double(sx)))*(std::min(y1,double(sy+1))-std::max(y0,double(sy)));
   const double expected=sum/((x1-x0)*(y1-y0));
   if(std::abs(reduced[(y*reduced_desc.Width+x)*4+ch]-expected)>1.1)throw std::runtime_error("Area downsample mismatch");
  }
  composition.compose(context.Get(),input.Get());auto result=read(input.Get());
  for(size_t i=0;i<pixels.size();i++)if(std::abs(int(result[i])-pixels[i])>1)throw std::runtime_error("Identity residual altered original");
  ++cases;
  for(UINT preserve:{0u,100u})for(bool guard:{false,true}){
   const float source[]={0.82f,0.90f,0.94f,1},answer[]={0.65f,0.95f,0.98f,1};
   ComPtr<ID3D11RenderTargetView> target;throw_if_failed(device->CreateRenderTargetView(input.Get(),nullptr,&target),"Source target");
   context->ClearRenderTargetView(target.Get(),source);auto original=read(input.Get());
   NrComposition c;c.initialize(device.Get(),desc,scale,preserve,guard);auto* model=c.prepare(context.Get(),input.Get());
   const auto before=read(model);ComPtr<ID3D11RenderTargetView> model_target;
   throw_if_failed(device->CreateRenderTargetView(model,nullptr,&model_target),"Model target");context->ClearRenderTargetView(model_target.Get(),answer);
   const auto after=read(model);c.compose(context.Get(),input.Get());auto out=read(input.Get());
   // The default bypass leaves the model's direct result intact.
   if(scale==100 && preserve==0 && !guard){if(out!=after)throw std::runtime_error("Default bypass mismatch");++cases;continue;}
   double delta[3];for(int k=0;k<3;k++)delta[k]=(int(after[k])-int(before[k]))/255.0;
   if(preserve){const double l=delta[0]*0.0722+delta[1]*0.7152+delta[2]*0.2126;for(auto& d:delta)d=l;}
   double maxc=std::max({original[0],original[1],original[2]})/255.0;
   double t=std::clamp((maxc-.75)/.25,0.0,1.0),weight=guard?1-t*t*(3-2*t):1;
   for(size_t i=0;i<out.size();i+=4)for(int k=0;k<3;k++){
    const double expected=std::clamp(original[k]/255.0+delta[k]*weight,0.0,1.0)*255;
    if(std::abs(out[i+k]-expected)>2)throw std::runtime_error("Color/highlight composition mismatch");
   }++cases;
  }
 }
 std::cout<<cases<<" composition cases passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
