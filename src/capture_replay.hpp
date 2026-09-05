#pragma once
#include "capture_color.hpp"
#include <vector>
#include <stdexcept>
// Replay fixture: encode decoded RGB as 8-bit BT.709 limited 4:2:0.
// P010 stores those values in its high bits, not a claim of 10-bit source coverage.
inline std::vector<uint8_t> planar_replay(std::vector<uint8_t>& rgb,UINT w,UINT h,bool p010){
 if(w%2 || h%2)throw std::runtime_error("Planar replay requires even dimensions");
 std::vector<uint8_t> yuv(static_cast<size_t>(w)*h*3/2);
 auto byte=[](int v){return uint8_t(std::clamp(v,0,255));};
 for(UINT y=0;y<h;y+=2)for(UINT x=0;x<w;x+=2){
  int us=0,vs=0;
  for(UINT dy=0;dy<2;dy++)for(UINT dx=0;dx<2;dx++){
   const auto i=static_cast<size_t>(y+dy)*w+x+dx;const auto* p=rgb.data()+i*4;int b=p[0],g=p[1],r=p[2];
   yuv[i]=byte(16+((47*r+157*g+16*b+128)>>8));
   us+=128+((-26*r-87*g+112*b+128)>>8);vs+=128+((112*r-102*g-10*b+128)>>8);
  }
  const size_t uv=static_cast<size_t>(w)*h+y/2*w+x;yuv[uv]=byte((us+2)/4);yuv[uv+1]=byte((vs+2)/4);
 }
 CaptureColor color;
 for(UINT y=0;y<h;y++)for(UINT x=0;x<w;x++){const size_t i=static_cast<size_t>(y)*w+x,uv=static_cast<size_t>(w)*h+y/2*w+(x&~1u);color.yuv(yuv[i],yuv[uv],yuv[uv+1],rgb.data()+i*4);}
 if(!p010)return yuv;
 std::vector<uint8_t> words(yuv.size()*2);for(size_t i=0;i<yuv.size();i++)words[i*2+1]=yuv[i];return words;
}
