# DLSS NR Capture

NR 켜기/끄기: **F10** 또는 Neural Rendering 메뉴의 **Toggle DLSS Neural Rendering**. 메뉴 체크 표시로 활성 상태를 확인할 수 있으며 설정은 다음 실행에도 유지됩니다.

Windows용 저지연 캡처·Neural Rendering 실험 애플리케이션입니다. 현재 첫 번째
마일스톤은 캡처 장치에서 프레임을 받아 지연이 누적되지 않는 latest-frame 방식으로
표시하는 실행 가능한 기반을 제공합니다.

현재 포함된 기능:

- Media Foundation 비디오 캡처 장치 탐색 및 캡처카드 우선 자동 연결
- 장치별 지원 해상도, 프레임률, 모든 네이티브 포맷 열거 및 RGB24/RGB32/NV12/P010/YUY2/UYVY/MJPG 선택 지원
- 마우스 휠과 키보드로 이동 가능한 Video mode 드롭다운
- Video format 및 Resolution 독립 메뉴; 장치가 지원하는 가장 가까운 유효 조합으로 전환
- `MF_LOW_LATENCY` 비동기 프레임 수신
- D3D11 flip-model swap chain 출력
- 처리 지연 시 오래된 프레임을 버리는 latest-frame 큐
- 교체 가능한 `IFrameProcessor` 경계
- 외부 또는 유출 NVIDIA 런타임을 저장소에 포함하지 않는 구조
- `Neural Rendering` 메뉴에서 NR 활성화, 시간축 처리, Style 0-3, Preset 1-4, 강도 및 출력 대기시간 선택
- NR 옵션을 사용자 설정에 저장하고 다음 실행 시 자동 복원
- View > Always on top으로 창을 항상 위에 표시하고 선택 상태 자동 복원
- View > Size window to capture resolution로 캡처 해상도에 맞춘 창 크기 자동 조절 및 상태 복원 (Per-Monitor DPI V2 및 Windows 배율 대응)
- `nvngx_dlssnr.dll` 누락 시 오류창을 표시하고 패스스루 유지
- NR 초기화·처리 실패 시 브리지의 상세 오류를 표시하고 안전하게 패스스루로 전환
- 창 제목에 프레임 도착→표시 지연과 latest-frame 교체 드롭 수 실시간 표시
- NR 입력·출력 공유 텍스처 분리 및 프레임 정렬: 실시간 NR 완성 프레임을 직접 표시해 최신 원본과 이전 보정값을 섞을 때 생기는 잔상 방지
- 선택 FPS에 맞춰 NR 속도 판정 자동 조정: 프레임 예산의 90% 이하에서 활성화, 150% 이상에서 저속 판정. 연속 결과 기준도 FPS에 비례하며 60FPS에서는 기존 30회·15ms / 60회·25ms를 유지. 짧은 갱신 공백에는 마지막 NR 프레임을 최대 100ms 유지
- NR이 너무 느려 원본으로 복귀한 동안에는 창·전체 화면 영상 위에 경고 오버레이 표시
- NR 프레임의 입력, Optical Flow, GPU 준비·실행, 출력 단계별 시간을 창 제목에 표시
- NVOF 축소 Flow를 D3D12 compute shader로 전체 해상도 모션 벡터에 확장해 CPU 병목 제거
- DLSS readback에서 채널 감지, 히스토리 마스크 및 최종 BGRA8 프레임 생성을 한 번에 처리해 중간 float 출력 제거
- D3D12가 공유 BGRA8 보정 render target을 생성하고 D3D11 worker가 GPU 복사해 정상 프레임의 CPU readback·재업로드 제거
- BGRA8 입력을 작은 upload buffer로 전달하고 D3D12 compute shader에서 RGBA16F DLSS 입력으로 변환
- NVOF는 worker의 D3D11 BGRA 텍스처를 GPU에서 직접 복사해 CPU RGB·luma 변환과 재업로드 제거
- 카메라 회전은 NVOF temporal motion에 맡기고 CPU 평행이동 분석의 reject-all·타일 마스크를 NR에 적용하지 않아 회전 중 history reset 깜빡임 방지
- 메뉴처럼 카메라 이동 없이 넓은 화면 영역이 바뀌는 전환은 별도로 감지해 NR history를 한 번만 초기화하고 30프레임 쿨다운으로 반복 reset 방지

기본 처리 백엔드는 `Passthrough`이며, 사용자가 별도 제공한 호환 런타임이 있을 때만
선택적으로 DLSS Neural Rendering 브리지를 활성화합니다. GPU 작업 프로세스, 공유 텍스처,
카메라 모션 분석, 장면 전환 및 히스토리 거부 처리가 구현되어 있습니다.

## 빌드

Visual Studio 2026의 C++ Build Tools와 Windows SDK가 필요합니다.

```powershell
cmake -S . -B build -G 'Visual Studio 18 2026' -A x64
cmake --build build --config Release
```

실행 파일은 `build/Release/dlss-nr-capture.exe`에 생성됩니다. 상단의 `Capture device`와
`Video mode` 메뉴에서 장치 및 `해상도 @ FPS — 픽셀 포맷` 조합을 선택할 수 있습니다.
`Esc`로 종료합니다.

## 안전 및 배포 원칙

이 프로젝트는 NVIDIA의 비공개·유출·수정 런타임을 재배포하지 않습니다. 향후 NR
어댑터도 사용자가 제공한 런타임을 별도 프로세스에서 검증하고 로드하는 방식으로
구현합니다. 런타임이 없거나 초기화에 실패하면 패스스루로 동작해야 합니다.

## 예정된 처리 파이프라인

```text
Capture (D3D11 texture)
  -> D3D11/D3D12 shared resource
  -> forward/backward optical flow
  -> global camera model + residual/confidence
  -> scene-cut/history control
  -> optional depth-aware multi-plane correction
  -> NR runtime adapter
  -> DirectComposition/swap-chain output
  -> optional shared-texture OBS output
```


## 선택적 DLSS NR 런타임

빌드하면 공개 MIT 브리지와 호출 보조 모듈이 다음 위치에 생성됩니다.

```text
build/Release/nr-runtime/dlss5nr_bridge.dll
build/Release/nr-runtime/caller/nvngx.dll_comfy.dll
```

실제 Neural Rendering을 사용하려면 사용자가 합법적으로 취득한 호환
`nvngx_dlssnr.dll`을 `build/Release/nr-runtime/`에 직접 배치해야 합니다. NVIDIA의
`_nvngx.dll`, `nvngx_dlssnr.dll`, SDK 헤더는 이 저장소와 빌드 결과에 포함되지
않습니다. 런타임이 없으면 메뉴에서 활성화할 때 설치 위치를 안내하는 오류창이 표시되며,
초기화 또는 처리에 실패하면 작업 프로세스는 패스스루로 폴백합니다.

공개 브리지의 저작권과 라이선스는 `THIRD_PARTY_NOTICES.md` 및
`third_party/comfyui_dlss5_nr/LICENSE`를 참조하십시오.
## 회귀 테스트

`View > Performance overlay`는 좌측 하단에 실측 캡쳐/표시/워커 출력 FPS,
NR 상태와 처리시간 EMA, 앱 내부 지연 및 누적 드롭 수를 표시합니다. 선택은 저장됩니다.
FPS는 약 1초간의 카운터 변화량이며, 워커 FPS는 NR OFF 시 패스스루도 포함합니다.
앱 지연은 캡쳐 콜백 도착부터 Present 반환까지로, 콘솔·캡쳐카드·모니터 지연을 포함한
전체 입력 지연이 아닙니다. 표시는 영상 프레임 도착 시 갱신되며 입력이 멈추면 마지막
표본이 남습니다. 작은 창(240 DIP 미만)에서는 경고와 겹치지 않도록 숨깁니다.

문제 발생 시 `View > Copy diagnostics to clipboard`를 선택하면 캡쳐 모드,
NR 설정과 상태, 프레임 카운터, 처리시간(us), 속도 판정 기준을 복사합니다.
보고서는 인접 프레임의 값이 섞일 수 있는 실시간 표본이며 영상 자체는 포함하지 않습니다.
복사 완료 알림은 검정 배경/노란 글씨로 1초 유지 후 0.5초 동안 사라집니다.

Release 빌드와 전체 회귀 테스트를 한 번에 실행합니다.

```powershell
cmake --build build --config Release --target regression
```

또는 이미 빌드된 결과에 대해 CTest만 다시 실행할 수 있습니다.

```powershell
ctest --test-dir build -C Release --output-on-failure
```

테스트 묶음은 프로토콜·모션 분석·어댑터 ABI, 필수 산출물과 독점 NVIDIA DLL 미포함,
앱 실행·메뉴·설정 저장 및 재실행 복원을 검사합니다. 앱 테스트는
`Software\DlssNrCapture\Tests\` 아래의 임시 레지스트리 키를 사용하고 종료 시 제거하므로
일반 사용자 설정을 변경하지 않습니다.
정상 종료 시 워커가 남지 않는지, 전체 화면 진입/복귀 시 메뉴와 창 테두리가
복원되는지, 재실행 후 설정 체크 표시와 진단 메뉴가 유지되는지도 검사합니다.
진단 메뉴는 존재 여부만 확인하며 테스트에서 사용자 클립보드를 변경하지 않습니다.
워커는 앱 전용 kill-on-close Job Object에 연결되어 앱 강제 종료 시에도 정리됩니다.
테스트는 장치 없이도 자식 프로세스 종료 정책을 검증하며, 캡쳐 워커가 실행 가능한
환경에서는 앱 강제 종료 후 실제 워커가 남지 않는지도 추가 검사합니다.
