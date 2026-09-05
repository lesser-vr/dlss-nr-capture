#include "common.hpp"
#include "shared_copy_completion.hpp"
#include "benchmark_stats.hpp"
#include <iostream>
#include <vector>
#include <chrono>
int main(){try{
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
 throw_if_failed(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"device");
 for(auto size:{std::pair{1920u,1080u},std::pair{2560u,1440u},std::pair{3840u,2160u}}){
  D3D11_TEXTURE2D_DESC d{};d.Width=size.first;d.Height=size.second;d.ArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
  ComPtr<ID3D11Texture2D> a,b;throw_if_failed(device->CreateTexture2D(&d,nullptr,&a),"a");throw_if_failed(device->CreateTexture2D(&d,nullptr,&b),"b");
  for(bool event:{false,true,true,false}){
   SharedCopyCompletion wait(event);std::vector<uint64_t> samples;
   FILETIME c,e,k0,u0,k1,u1;GetThreadTimes(GetCurrentThread(),&c,&e,&k0,&u0);
   for(int i=0;i<1030;i++){auto start=std::chrono::steady_clock::now();context->CopyResource(b.Get(),a.Get());throw_if_failed(wait.wait(context.Get()),"completion");if(i>=30)samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());}
   GetThreadTimes(GetCurrentThread(),&c,&e,&k1,&u1);
   auto ft=[](FILETIME f){return (uint64_t(f.dwHighDateTime)<<32)|f.dwLowDateTime;};auto s=benchmark_stats(samples);
   std::cout<<size.first<<'x'<<size.second<<",event="<<wait.uses_event()<<",mean_us="<<s.mean<<",p95_us="<<s.p95<<",p99_us="<<s.p99<<",thread_cpu_ms="<<(ft(k1)+ft(u1)-ft(k0)-ft(u0))/10000.0<<'\n';
  }
 }
 return 0;
}catch(const std::exception& e){std::cerr<<e.what();return 1;}}
