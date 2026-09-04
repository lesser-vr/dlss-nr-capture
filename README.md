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

### 영상 파일 성능 벤치마크

앱/다른 GPU 작업을 종료한 뒤, **프로세스 시간 제한을 제공하는 스크립트**로 실행합니다.
플레이 중인 콘솔이나 캡쳐카드는 필요하지 않습니다.

```powershell
.\tools\benchmark.ps1 -InputVideo 'D:\clips\camera-pan.mp4'
# 입력 영상 없이 합성 패턴으로 실행
.\tools\benchmark.ps1 -Synthetic -Warmup 30 -Frames 90
```

기본값은 앞 120프레임 워밍업 후 300프레임 측정입니다. 따라서 60FPS 영상은 최소
7초가 필요합니다. `-Style`, `-Preset`, `-Intensity`, `-Temporal`, `-Warmup`, `-Frames`,
`-OutputDir`, `-ReleaseDir`, `-TimeoutSeconds`(기본 300초)를 지정할 수 있습니다.
Media Foundation이 디코딩 가능한 로컬 영상 파일을 지원하며, 중간 해상도 변경은 거부합니다.
짧은 영상은 자동 반복하지 않고 `incomplete`로 보고합니다. 기존 결과 폴더는 덮어쓰지 않습니다.

기본 결과 위치는 `build/Release/benchmark-날짜-고유ID/`입니다.

- `frames.csv`: 프레임 시각, 워밍업 여부, 성공 여부, 디코딩·분석·업로드·NR 단계별 시간(us)
- `summary.json`: 워밍업 제외 평균/p95/p99/최대 처리시간, 순차 처리량, 원본 FPS 예산 초과 횟수와 설정
- `manifest.json`: 영상/실행파일/런타임 SHA-256, 실행 시각, 프로세스 종료 및 강제 정리 여부

동일 영상 해시·설정·프레임 구간으로 비교해야 합니다. 처리량은 오프라인 순차 처리의
최대 처리 용량 추정이며 실제 게임 FPS가 아닙니다. 입력/디스플레이 지연, 실제 드롭률,
폴백률과 자동 화질 점수는 측정하지 않아 `null`로 표시합니다. 워밍업 프레임은 CSV에 남습니다.
NR OFF/ON 앱 상태와 무관한 별도 실행이며 앱 사용자 설정을 변경하지 않습니다.

일부 temporal NR 실행은 측정 완료 후 어댑터 정리에서 멈출 수 있습니다. 스크립트는
유효한 보고서 저장 후 5초 동안 종료되지 않으면 **해당 벤치마크 프로세스만** 종료하며
`forced_cleanup_after_report`를 기록합니다. 전체 시간 초과는 성공으로 처리하지 않습니다.
전체 해상도 출력 저장과 자동 화질 합격/불합격 판정은 후속 작업입니다.

### 화질 회귀 검토용 출력 비교

같은 영상으로 기준/후보 출력을 생성한 뒤 비교합니다. 현재 단계에서는 **320×180 RGB
축소 프레임**을 매 프레임 저장합니다. `-CaptureOutput` 사용 시 GPU readback/디스크 출력이
추가되므로 성능 비교용 실행과 분리해야 합니다(`performance_comparable: false`).

```powershell
.\tools\benchmark.ps1 -InputVideo '.\tests\gaming test sample vd.mp4' -CaptureOutput -Warmup 30 -Frames 1204 -OutputDir '.\build\quality-baseline'
# 변경된 빌드 또는 반복 실행
.\tools\benchmark.ps1 -InputVideo '.\tests\gaming test sample vd.mp4' -CaptureOutput -Warmup 30 -Frames 1204 -OutputDir '.\build\quality-candidate'
.\tools\compare-quality.ps1 -Baseline '.\build\quality-baseline' -Candidate '.\build\quality-candidate' -OutputDir '.\build\quality-comparison'
```

- 측정 구간의 `input.rgb`/`output.rgb`를 저장합니다. 워밍업은 출력 파일에서 제외되며
  각 프레임은 `frames.csv`의 원본 시각과 대응합니다. 1204프레임은 실행당 약 397 MiB입니다.
- 비교 도구는 영상 해시, 설정, GPU, 프레임 수/시각 및 디코딩된 입력 픽셀 일치를 검증합니다.
- `comparison.csv`는 기준/후보 RGB 평균 절대 차이(MAE), 입력 변화, 시간적 잔차 변화와
  검토 플래그를 기록합니다. 잔차는 NR 출력에서 입력을 뺀 값이며 **모션 보정은 하지 않습니다**.
- `report.html`에서 차이가 큰 최대 20프레임의 원본 시각을 확인할 수 있습니다.
  ffmpeg가 있으면 원본/기준/후보를 나란히 배치한 무음 `comparison.mp4`도 생성합니다.
  ffmpeg가 없거나 `-NoVideo`를 지정하면 수치/HTML만 생성합니다.
- MAE > 5, 후보의 시간적 잔차 증가 > 3, 거의 정적인 입력에서 잔차 변화 > 3을
  검토 대상으로 표시합니다(RGB 0–255 단위의 초기 경험적 기준). **화질 저하 판정이 아닙니다.**
  카메라/물체 움직임, NR의 의도적 외관 변화, 점 샘플링도 큰 차이를 만들 수 있습니다.
  작은 잔상·텍스처·HDR 색 정확도는 이 축소 SDR 경로만으로 검증할 수 없습니다.
- MP4는 검토용 손실 압축 영상이고 지표는 압축 전 RGB 파일로 계산합니다.
  `quality_pass`는 미판정(null)입니다. 처음에는 같은 빌드 반복 결과로 변동 폭부터 확인하세요.

원본 게임 영상 `tests/gaming test sample vd.mp4`는 Git LFS로 관리합니다.
생성 결과와 독점 NR DLL은 Git/LFS에 포함하지 않으며, 게임 영상은 실행 파일 배포 패키지에도 넣지 않습니다.

Git LFS가 설치된 환경에서 저장소를 복제하면 영상 본체도 내려받습니다. 이미 복제했거나
LFS 다운로드를 생략한 환경에서는 저장소 폴더에서 아래 명령을 실행하세요.

```powershell
git lfs install --local
git lfs pull
git lfs ls-files
```

Git에는 영상의 SHA-256과 크기를 담은 작은 포인터가 저장되고 실제 영상은 LFS에 저장됩니다.
일반 `git push` 시 설치된 LFS pre-push hook이 영상 본체를 먼저 업로드합니다.
GitHub Actions는 `lfs: true`로 영상까지 체크아웃합니다. LFS 다운로드에 실패해 포인터만
남았다면 영상 디코딩 테스트는 실패하므로 `git lfs pull`로 복원해야 합니다.

### 자동 회귀 검사

GPU fence 입력 전달 실험은 `DLSS_NR_EXPERIMENTAL_GPU_FENCE_INPUT` CMake 옵션으로
분리했습니다(기본 OFF). 픽셀 일치 테스트는 통과했지만 전체 NR 처리시간 개선은
확인되지 않아 기본 동기화 경로는 유지합니다. 측정 결과는
[GPU fence 평가](docs/gpu-fence-evaluation.md), 이후 작업 순서는
[작업 계획](docs/roadmap.md)을 참조하세요.

NR 워커→D3D12 입력은 공유 GPU 텍스처로 전달하며 정상 처리 중 CPU 픽셀 왕복을 하지 않습니다.
첫 프레임의 출력 채널 순서 판별에는 CPU 참조 픽셀이 필요합니다. 공유 입력 복사는
완료 확인 후 사용하고, D3D12 처리 완료 후 재사용하는 직렬 경로입니다.
캡쳐·분석 단계 전체가 GPU 전용으로 바뀐 것은 아닙니다.

`regression.gpu-input`은 NVIDIA 런타임 없이 WARP로 공유 입력 픽셀 일치, 반복 갱신 및
크기 변경을 검사합니다. 로컬 NR 런타임이 있을 때는 아래 명령으로 1080p 합성 프레임
45개와 히스토리 리셋·temporal 모드 전환을 검증할 수 있습니다(화질 평가는 별도).

```powershell
& .\build-vs2026-async\Release\dlss-nr-gpu-input-test.exe "$PWD\build-vs2026-async\Release\dlss-nr-adapter-bridge.dll"
```

`View > Performance overlay`는 좌측 하단에 실측 캡쳐/표시/워커 출력 FPS,
NR 상태와 처리시간 EMA, 앱 내부 지연 및 누적 드롭 수를 표시합니다. 선택은 저장됩니다.
FPS는 약 1초간의 카운터 변화량이며, 워커 FPS는 NR OFF 시 패스스루도 포함합니다.
앱 지연은 캡쳐 콜백 도착부터 Present 반환까지로, 콘솔·캡쳐카드·모니터 지연을 포함한
전체 입력 지연이 아닙니다. 표시는 영상 프레임과 무관하게 타이머에서도 갱신되어 입력이
멈추면 FPS가 0으로 내려갑니다. 입력 중단 중 지연은 N/A로 표시합니다.
작은 창(240 DIP 미만)에서는 경고와 겹치지 않도록 숨깁니다.

`View > Refresh devices`는 비디오/오디오 입력 목록을 다시 읽습니다. 앱 실행 후 새 장치를
연결했거나 USB 포트 변경으로 식별자가 바뀌었을 때 재시작 없이 메뉴에서 선택할 수 있습니다.
기존 캡쳐·NR 워커·포맷 설정은 재시작하거나 변경하지 않으며 새 장치로 자동 전환하지 않습니다.
연결이 끊긴 현재 비디오 선택은 복구 대상으로 목록에 유지할 수 있습니다.

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
워커 상태 검사는 캡쳐 프레임 수와 무관하게 500ms UI 타이머로 실행합니다.
종료/생성 실패는 최소 5초 간격으로 재시도하며, 최초 heartbeat에는 30초 초기화 유예를 줍니다.
이후 heartbeat가 2초 넘게 끊기면 재시작합니다(재시작 간격 제한 적용).
영상 프레임 전달을 막은 상태에서 워커를 종료해도 복구되는지 검사하고, 재시도 횟수와
마지막 재시작 오류는 진단 복사에 포함합니다.

캡쳐 오류 또는 8초간 프레임이 오지 않는 경우 최소 5초 간격으로 같은 장치를 다시 찾습니다.
프레임 수신 후 2초간 입력이 멈추거나 오류가 발생하면 기존 오류 스타일로
`CAPTURE INTERRUPTED - WAITING FOR INPUT`을 표시합니다(최초 입력에는 8초 유예).
입력 중단 경고는 NR 상태 알림보다 우선하며 새 입력이 오면 해제됩니다.
대기 화면은 마지막 원본을 다시 그리지만 NR에 재전송하거나 영상 FPS에 합산하지 않습니다.
장치의 Media Foundation symbolic link와 이전 포맷/해상도/FPS가 일치할 때만 자동 재연결합니다.
상하 반전 선택은 유지하며, 다른 장치나 낮은 해상도로 자동 전환하지 않습니다.
장치/모드가 없으면 창 제목에 대기 사유를 표시하고 계속 재시도합니다. 수동 장치 선택은
이전 복구 대상을 취소합니다. 진단 복사에는 재연결 시도 횟수와 마지막 오류가 포함됩니다.
무신호 화면도 프레임이 도착하는 한 정상 연결로 취급하며, USB 포트 변경 등으로 식별자가
달라진 경우 `View > Refresh devices`를 실행한 뒤 장치를 다시 선택할 수 있습니다. 지원 모드만 달라진
경우 갱신된 모드 메뉴에서 직접 선택할 수 있습니다.
회귀 테스트는 격리된 테스트 앱에 캡쳐 오류를 주입해 정확한 모드와 반전 설정 복원을
검사합니다. 실제 USB/HDMI 분리·연결과 드라이버별 복구는 실장치 확인이 필요합니다.

오디오는 캡쳐/출력 오류 시에도 재생 대기 버퍼를 리셋하고 준비 해제한 뒤 출력 장치를
닫습니다. 캡쳐 패킷도 예외 발생 시 반환합니다. 회귀 테스트는 실제 녹음·재생 없이
출력 준비/전송/버퍼 회수 실패와 정상 종료의 정리 순서, PCM 복사 및 무음 처리를 검사합니다.
드라이버가 리셋 후에도 버퍼 반환을 거부하면 잘못된 메모리 접근을 막기 위해 해당 버퍼와
핸들을 프로세스 종료까지 남깁니다.

선택한 오디오 입력에서 오류가 발생하면 최소 5초 간격으로 자동 재연결합니다.
Windows endpoint ID를 이름과 함께 저장해 재실행해도 정확히 같은 입력만 선택하며
이름이 같은 다른 입력으로 임의 전환하지 않습니다. 시작 시 장치가 없어도 선택을 유지하고
연결을 기다립니다. 비디오 장치가 없는 경우에도 오디오 복원은 수행합니다.
이전 이름 전용 설정은 같은 이름의 입력이 정확히 하나일 때 ID로 전환하며, 중복되면
수동 선택을 기다립니다. Off 선택은 저장된 이름과 ID를 모두 지웁니다.
장치가 없으면 오디오 메뉴를 현재 목록으로 갱신하고 선택 이름을 유지한 채 대기합니다.
Off 또는 다른 입력을 선택하면 이전 복구 대상은 취소합니다. 진단 복사에는 선택 장치,
재연결 횟수와 마지막 오류가 포함됩니다. 이 복구는 오류 기반이며 무음 자체는 오류로
취급하지 않습니다. 출력은 기존과 같이 시스템 기본 출력입니다. 실제 오디오 장치 분리·연결과
드라이버별 복구는 실장치 확인이 필요합니다.
자동 테스트는 없는 오디오 endpoint를 지정해 재연결 대기, 선택 보존, Off 취소를 검사하며
실제 오디오를 녹음하거나 재생하지 않습니다.
