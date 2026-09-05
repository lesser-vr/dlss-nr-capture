# Capture stability and synchronization follow-up

Validated locally on 2026-09-05 with RTX 5090 and Live Gamer Ultra 2.1.
This is bounded regression coverage, not a long-play, physical device-loss or subjective quality certification.

## 1. Shared-copy completion

Shared buffers remain owned until GPU copies complete. D3D11 fence events replace busy polling where supported; event/query waits remain bounded to one second. Older interfaces use the original query fallback. Device removal is checked before and after event waits. Flush alone is not sufficient: the WARP ownership regression previously reproduced mismatched pixels and metadata without completion waiting.

The manual `dlss-nr-copy-timing` benchmark used ABBA query/event/event/query runs, 30 warm-up copies and 1,000 measured copies per run. Times below are ranges over the two runs, in microseconds. These are CPU-observed copy-and-wait timings, not GPU timestamps or end-to-end NR performance.

| Resolution | Query mean | Event mean | Query p99 | Event p99 | Query thread CPU ms | Event thread CPU ms |
|---|---:|---:|---:|---:|---:|---:|
| 1920x1080 | 50.5–51.2 | 71.5–72.9 | 78–79 | 119–121 | 62.5 | 0 |
| 2560x1440 | 58.1–60.6 | 80.5–84.0 | 88 | 122–129 | 46.9–62.5 | 31.3 |
| 3840x2160 | 64.0–65.0 | 83.5–85.4 | 88–93 | 121–131 | 62.5 | 31.3 |

Event waits trade about 20–25 microseconds per copy for less polling CPU use. Thread CPU accounting is coarse (about 15.6 ms here): zero means below the measurement resolution, not no CPU work. No real-time FPS gain is claimed.

## 2. SDR color and planar replay

Negotiated Media Foundation matrix/range metadata controls CPU and GPU conversion: BT.601/709, full/limited YUV and full/limited RGB. Missing metadata uses BT.709 limited YUV or full RGB and is labeled as assumed in diagnostics. PQ, HLG, BT.2020 and unsupported matrix/range values are rejected with a request to use SDR; this is not HDR tone mapping. A runtime type change is rechecked before processing.

Regression coverage includes 16 NV12/P010 matrix/range/flip combinations and two explicit RGB range cases, checked against independent reference values. All passed with at most three levels of allowed rounding tolerance.

`tools/benchmark.ps1 -CaptureFormat NV12` or `P010` encodes decoded video into a deterministic BT.709 limited planar fixture. Add `-GpuCapture` for production GPU conversion. Both routes archive the same canonical source reference; comparison tools reject mismatched capture formats. P010 fixtures currently contain 8-bit source values in a 10-bit container, not 10-bit/HDR source coverage.

The existing game fixture was replayed at full 2560x1440 resolution for 24 frames per route with NR enabled and no warm-up. On the RGB 0–255 scale:

| Format | CPU/GPU NR-output mean MAE | Maximum frame MAE | Review flags |
|---|---:|---:|---:|
| NV12 | 0.638294 | 0.829237 | 0 |
| P010 | 1.05784 | 1.18754 | 0 |

These are route differences, not ground-truth quality scores. Chroma reconstruction and rounding can differ; zero review flags is not a subjective quality pass. Reports are local under `build/color-NV12-compare-v1` and `build/color-P010-compare-v1`. Capture-output runs are not performance-comparable and their startup-inclusive FPS must not be interpreted as playability.

## 3. Recovery

The health timer detects a removed graphics device, stops capture/worker use, clears queued frames, recreates renderer/DXGI capture resources and restarts the selected mode. Failed attempts retry at bounded intervals. Application suspend stops capture, worker and audio; resume schedules graphics and audio recovery. Settings remain selected. Restoration notifications are only shown for a healthy capture restart.

The isolated live test covers an injected capture failure, graphics recreation and application suspend/resume messages. It does not unplug USB/HDMI, suspend Windows or deliberately reset the driver. Actual hardware loss, driver hangs and physical sleep/resume remain user-coordinated acceptance tests. A capture card generating valid no-signal frames is not necessarily a disconnected device.

## 4. GPU flip and analysis readback

Supported video processors mirror vertically before the analysis image and worker input are produced. In-flight frames with obsolete orientation are discarded. Unsupported mirror capability falls back to CPU; the history debug overlay still uses CPU. Actual NV12 flip/unflip stayed on the GPU path during the transition test.

The small analysis readback remains synchronous. An asynchronous previous-frame analysis path was not adopted: it would complicate same-frame metadata alignment and can add a frame of latency (16.7 ms at 60 Hz). This optional optimization needs separate profiling before accepting that tradeoff.

## 5. Experimental automatic A/V delay

Audio capture > Automatic A/V sync (experimental) is saved and defaults OFF. Capture timestamps are aligned to arrival QPC, and only successfully presented video observations update a smoothed app-side delay estimate. WASAPI packet QPC timestamps schedule audio; invalid timestamps fall back to arrival timing. The existing manual positive delay remains available, with total target delay capped at 200 ms. Queues are bounded and reset on discontinuities, mode/NR restart and option changes; stale video observations cease contributing delay.

This compensates an estimate of application video delay, not fixed capture-card, speaker or display latency. It does not resample for hardware clock drift and cannot guarantee physical lip-sync. Keep manual adjustment available and verify with an audiovisual flash/click reference when precise synchronization matters.

The real HDMI/Line In endpoint processed 6,706 packets through the approximately 68-second transition run. Tests covered 30/60 fps, 1080p/1440p, NV12/P010/RGB24/MJPG, GPU toggle/flip, history overlay, capture recovery, graphics recreation and application resume; the app exited cleanly. Packet delivery and recovery passed; audible quality and physical A/V offset were not measured.

## Validation commands

```powershell
cmake --build build-gpu-flow --config Release --target regression -j 8
./tests/live_nr_smoke.ps1 -AppPath ./build-gpu-flow/Release/dlss-nr-capture.exe -GpuCapture -DeviceName 'Live Gamer Ultra 2.1-Video' -AudioDeviceName 'HDMI/Line In(Live Gamer Ultra 2.1-Audio)' -Transitions -Seconds 8
```

GPU capture remains opt-in; GPU flow defaults are unchanged. Private NR runtime files are not redistributed. Long-play automation and release publication are outside this batch.
