#pragma once
#include <mfapi.h>
#include <algorithm>
#include <cstdint>
#include <string>
struct CaptureColor {
 bool bt601{}, full{}, assumed{true}, unsupported{}, hdr{}, rgb{};
 static CaptureColor from(uint32_t matrix,uint32_t range,uint32_t transfer,uint32_t primaries,bool rgb=false){
  CaptureColor c;c.rgb=rgb;c.bt601=matrix==MFVideoTransferMatrix_BT601;
  c.full=range==MFNominalRange_0_255 || (!range && rgb);
  c.assumed=!range || (!matrix && !rgb);
  c.hdr=transfer==MFVideoTransFunc_2084 || transfer==MFVideoTransFunc_HLG;
  c.unsupported=c.hdr || primaries==MFVideoPrimaries_BT2020 ||
    (matrix && matrix!=MFVideoTransferMatrix_BT601 && matrix!=MFVideoTransferMatrix_BT709) || range>MFNominalRange_16_235;
  return c;
 }
 static CaptureColor read(IMFMediaType* type,bool rgb){return from(
  MFGetAttributeUINT32(type,MF_MT_YUV_MATRIX,0),MFGetAttributeUINT32(type,MF_MT_VIDEO_NOMINAL_RANGE,0),
  MFGetAttributeUINT32(type,MF_MT_TRANSFER_FUNCTION,0),MFGetAttributeUINT32(type,MF_MT_VIDEO_PRIMARIES,0),rgb);}
 std::wstring label() const {return unsupported?(hdr?L"HDR unsupported: use SDR input":L"Unsupported colorimetry: use BT.601/709 SDR"):
  std::wstring(rgb?L"RGB ":bt601?L"BT.601 ":L"BT.709 ")+(full?L"full range":L"limited range")+(assumed?L" (metadata default)":L" (metadata)");}
 void yuv(uint8_t y,uint8_t u,uint8_t v,uint8_t* p) const {
  const int c=full?y:std::max(0,int(y)-16),d=int(u)-128,e=int(v)-128;
  const int scale=full?256:298;
  const int blue=full?(bt601?454:475):(bt601?516:541);
  const int gu=full?(bt601?88:48):(bt601?100:55),gv=full?(bt601?183:120):(bt601?208:136);
  const int red=full?(bt601?359:403):(bt601?409:459);
  p[0]=static_cast<uint8_t>(std::clamp((scale*c+blue*d+128)>>8,0,255));
  p[1]=static_cast<uint8_t>(std::clamp((scale*c-gu*d-gv*e+128)>>8,0,255));
  p[2]=static_cast<uint8_t>(std::clamp((scale*c+red*e+128)>>8,0,255));p[3]=255;
 }
};
