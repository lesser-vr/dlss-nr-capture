#pragma once
#include "common.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

// SDR-only experimental processing. All resources are worker-private.
class NrComposition {
    ComPtr<ID3D11Texture2D> original_, small_before_, small_nr_, result_;
    ComPtr<ID3D11ShaderResourceView> original_view_, before_view_, nr_view_;
    ComPtr<ID3D11RenderTargetView> before_target_, result_target_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
    UINT width_{},height_{},small_width_{},small_height_{};
    float preserve_{},guard_{};
    bool enabled_{};
    void draw(ID3D11DeviceContext* c, bool down) {
        const float settings[]={float(width_),float(height_),float(small_width_),float(small_height_),down?0.0f:1.0f,preserve_,guard_,0};
        c->UpdateSubresource(constants_.Get(),0,nullptr,settings,0,0);
        ID3D11Buffer* cb=constants_.Get();c->PSSetConstantBuffers(0,1,&cb);
        ID3D11ShaderResourceView* views[]={original_view_.Get(),down?nullptr:before_view_.Get(),down?nullptr:nr_view_.Get()};
        c->PSSetShaderResources(0,3,views);ID3D11SamplerState* sampler=sampler_.Get();c->PSSetSamplers(0,1,&sampler);
        auto* target=down?before_target_.Get():result_target_.Get();c->OMSetRenderTargets(1,&target,nullptr);
        D3D11_VIEWPORT viewport{0,0,float(down?small_width_:width_),float(down?small_height_:height_),0,1};c->RSSetViewports(1,&viewport);
        c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs_.Get(),nullptr,0);c->PSSetShader(ps_.Get(),nullptr,0);c->Draw(3,0);
        ID3D11ShaderResourceView* empty[3]{};c->PSSetShaderResources(0,3,empty);c->OMSetRenderTargets(0,nullptr,nullptr);
    }
public:
    D3D11_TEXTURE2D_DESC initialize(ID3D11Device* device,D3D11_TEXTURE2D_DESC desc,UINT scale,UINT preserve,bool guard) {
        width_=desc.Width;height_=desc.Height;
        scale=(scale==50 || scale==75)?scale:100;
        small_width_=scale==100?width_:std::max(2u,(width_*scale/100)&~1u);
        small_height_=scale==100?height_:std::max(2u,(height_*scale/100)&~1u);
        preserve_=std::min(preserve,100u)/100.0f;guard_=guard?1.0f:0.0f;
        enabled_=scale!=100 || preserve!=0 || guard;
        if(!enabled_)return desc;
        desc.MiscFlags=0;desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        auto make=[&](UINT w,UINT h,ComPtr<ID3D11Texture2D>& t,ComPtr<ID3D11ShaderResourceView>* view,ComPtr<ID3D11RenderTargetView>* target){
            auto d=desc;d.Width=w;d.Height=h;
            throw_if_failed(device->CreateTexture2D(&d,nullptr,&t),"Create creative texture");
            if(view)throw_if_failed(device->CreateShaderResourceView(t.Get(),nullptr,view->ReleaseAndGetAddressOf()),"Create creative view");
            if(target)throw_if_failed(device->CreateRenderTargetView(t.Get(),nullptr,target->ReleaseAndGetAddressOf()),"Create creative target");
        };
        make(width_,height_,original_,&original_view_,nullptr);
        make(width_,height_,result_,nullptr,&result_target_);
        make(small_width_,small_height_,small_before_,&before_view_,&before_target_);
        make(small_width_,small_height_,small_nr_,&nr_view_,nullptr);
        const char* shader=R"(
Texture2D<float4> original:register(t0);Texture2D<float4> beforeNR:register(t1);Texture2D<float4> afterNR:register(t2);
SamplerState linearClamp:register(s0);
cbuffer Settings:register(b0){float2 fullSize;float2 smallSize;float mode;float preserve;float guard;float padding;};
float4 vs(uint id:SV_VertexID):SV_POSITION{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}
float4 ps(float4 pos:SV_POSITION):SV_TARGET{
 if(mode<0.5){
  float2 lo=floor(pos.xy)*fullSize/smallSize,hi=(floor(pos.xy)+1)*fullSize/smallSize;
  int2 start=int2(floor(lo));float3 sum=0;float area=0;
  [unroll]for(int y=0;y<3;y++)[unroll]for(int x=0;x<3;x++){
   int2 p=start+int2(x,y);float2 overlap=max(0,min(hi,float2(p)+1)-max(lo,float2(p)));
   float weight=overlap.x*overlap.y;
   sum+=original.Load(int3(clamp(p,int2(0,0),int2(fullSize)-1),0)).rgb*weight;area+=weight;
  }
  return float4(sum/max(area,0.0001),1);
 }
 float2 uv=pos.xy/fullSize;
 float3 base=original.Load(int3(int2(pos.xy),0)).rgb;
 float3 delta=afterNR.SampleLevel(linearClamp,uv,0).rgb-beforeNR.SampleLevel(linearClamp,uv,0).rgb;
 float luminance=dot(delta,float3(0.2126,0.7152,0.0722));
 delta=lerp(delta,luminance.xxx,preserve);
 float highlight=smoothstep(0.75,1.0,max(base.r,max(base.g,base.b)));
 delta*=1-guard*highlight;
 return float4(saturate(base+delta),1);
})";
        ComPtr<ID3DBlob> v,p,error;
        throw_if_failed(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&v,&error),"Compile creative vertex shader");
        throw_if_failed(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&p,&error),"Compile creative pixel shader");
        throw_if_failed(device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs_),"Create creative vertex shader");
        throw_if_failed(device->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps_),"Create creative pixel shader");
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=32;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        throw_if_failed(device->CreateBuffer(&cb,nullptr,&constants_),"Create creative constants");
        D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
        throw_if_failed(device->CreateSamplerState(&sampler,&sampler_),"Create creative sampler");
        desc.Width=small_width_;desc.Height=small_height_;return desc;
    }
    ID3D11Texture2D* prepare(ID3D11DeviceContext* c,ID3D11Texture2D* full){
        if(!enabled_)return full;
        c->CopyResource(original_.Get(),full);draw(c,true);c->CopyResource(small_nr_.Get(),small_before_.Get());return small_nr_.Get();
    }
    void compose(ID3D11DeviceContext* c,ID3D11Texture2D* full){if(enabled_){draw(c,false);c->CopyResource(full,result_.Get());}}
};
