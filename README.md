# DLSS NR Capture

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
- 창 제목에 프레임 도착→표시 지연과 latest-frame 교체 드롭 수 실시간 표시

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
