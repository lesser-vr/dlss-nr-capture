# GPU capture and bounded worker pipeline (2026-09-05)

## Follow-up items 1, 3, 4 and 5

1. The previous `0xC00D3704` is `MF_E_HW_MFT_FAILED_START_STREAMING`.
   Another capture-app instance was present during the earlier failure; this
   is consistent with device contention, not proof of the only possible cause.
   Reconnect smoke passed with exclusive device use. The test now rejects an
   already-running capture app and accepts successful recovery after multiple
   retries. It preserves exact mode/flip settings. Physical unplug/replug is
   not simulated by this injected-error test. GitHub run 33935242057 passed
   for the previously pushed de7c93f revision; that run does not validate the
   subsequent change set below.
3. GPU coarse-flow default decision: ON for new CMake builds after the CPU
   transfer recovery checks below. Existing caches retain their selected value;
   use `DLSS_NR_EXPERIMENTAL_GPU_FLOW=ON` to update an old OFF cache. Other
   adapters remain unvalidated; the legacy option name is retained.
4. GPU-native capture is implemented as an opt-in, saved Processing menu
   option. `DLSS_NR_GPU_CAPTURE=0/1` overrides the saved choice at startup for
   isolated testing. The renderer device is supplied through Media Foundation's
   DXGI device manager. DXGI samples preserve their array subresource; the video
   processor converts to owned BGRA surfaces and a 96-pixel-wide analysis image.
   Only the small image is read back, synchronously. There is still GPU copying
   and synchronization: this is NOT end-to-end zero-copy or readback-free.
5. The worker now has separate shared input, private processing and shared
   output textures. Input pixels and their analysis payload are published under
   the same keyed mutex, using protocol 8. NR no longer holds the output mutex
   during evaluation. One occupied output causes newer completed results to be
   dropped, not queued. Output sequence, processing time and completion time
   travel with the corresponding output; old output cannot become fresh merely
   because presentation resumed. Diagnostics expose published/dropped counts.
   NR itself remains sequential; this does not issue concurrent NR evaluations.

## Capture compatibility

- Three leased conversion surfaces bound outstanding GPU capture frames.
  The UI/latest-frame queue retain ownership; a leased surface is never reused.
- Unsupported system-memory samples, unavailable video conversion, pool
  exhaustion and coded-size mismatch use the existing CPU conversion path.
  Reader creation retries without the device manager if the GPU request fails.
- Vertical flip and rejected-history overlay use the CPU path. RGB24 and MJPG
  normally require CPU decoding/conversion. No format is silently changed.
- SDR planar conversion uses BT.709 limited-range input and full-range RGB
  output, matching existing CPU assumptions. This is not HDR tone mapping or
  automatic handling of arbitrary source colorimetry.
- `capture GPU/CPU` in the title reports cumulative delivered-path counters.
  Small-image blackout probes retain their sparse/heuristic limitation.
- Reduced GPU analysis uses video-processor resampling, unlike CPU point
  sampling; live temporal behavior therefore still needs visual review.

Media Foundation integration follows Microsoft's
[D3D manager attribute](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-source-reader-d3d-manager),
[multithread protection guidance](https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation)
and [subresource API](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfdxgibuffer-getsubresourceindex).

## GPU flow resource cost and default policy

GPU flow is a transfer-path optimization, not an additional heavy AI model.
Both paths already calculate optical flow on the GPU using NVOF. The baseline
reads the coarse vectors back to CPU memory and uploads them again; GPU flow
keeps that handoff on the GPU. It is separate from GPU-native capture, which
optimizes the capture-input path.

Shared GPU resources, copies and synchronization still have a cost. The benefit
is removing the coarse-vector CPU round-trip and associated waiting, not making
all GPU work free. NR inference remains the main GPU workload. The measurements
below show processing time, not GPU utilization, power consumption or a measured
VRAM delta; no universal minimum GPU specification follows from them.

On the tested RTX 5090, the two-run average NR time changed from about 6.92 ms
to 6.09 ms (about 12% less). Shared-flow allocation or copy failure now latches
CPU vector transfer for the remainder of the NR session, preserving the current
NVOF result and frame pair without evaluating the frame twice. NR stays enabled.
A new NR session retries GPU sharing. Shader descriptors and upload buffers are
rebuilt for the CPU route. The old shared resource is not reused after failure.

This recovery permits the new-build default ON. It does not guarantee recovery
from a hung/removed GPU: CPU transfer still needs that GPU. Existing worker
watchdog and original-video fallback remain the last resort. No multi-hour
stability or other-GPU compatibility guarantee follows from the local tests.

## Measurement

RTX 5090, supplied 2560x1440 60fps game fixture, style 1/preset 3/intensity 100,
temporal ON, 30 warmup + 1200 measured frames per run, sequential ABBA order:

| Run | NR mean us | p95 us | p99 us | Offline pipeline capacity FPS |
|---|---:|---:|---:|---:|
| CPU flow A1 | 6915.651 | 7151 | 8021 | 44.224 |
| GPU flow B1 | 6112.545 | 6328 | 6789 | 45.645 |
| GPU flow B2 | 6060.907 | 6235 | 6677 | 46.164 |
| CPU flow A2 | 6923.023 | 7162 | 8102 | 44.315 |

Mean NR time improves about 12%; offline pipeline throughput improves about
3.7%. Each run has zero failures and one frame above the 16.67ms processing
budget. These are not live capture FPS, input lag, a long-play soak or a visual
quality verdict. Reports/manifests are in ignored `build/long-flow-{a1,b1,b2,a2}`.

## Automated coverage

- Nine regression suites, adding actual worker IPC/backpressure tests and
  hardware video-converter tests. Hardware-only cases explicitly skip with
  code 77 if no hardware D3D device is available; a skip is not hardware proof.
- Worker test: 80 paired pixel/metadata frames, occupied output, bounded drops,
  resumption without stale output, invalid input not published.
- Capture test: BGRA color, full/small dimensions, array slice, three leases,
  pool exhaustion/reuse, short analysis rejection, NV12/P010 SDR black/white.
- Physical Live Gamer Ultra 2.1 NV12 1920x1080@60 smoke observed 66 GPU frames
  and zero CPU frames; settings/reconnect and vertical-flip preservation passed.
- Actual NR temporal OFF/ON repeated six isolated processes, all exited without
  forced cleanup. Full-resolution flow-comparison artifacts are under
  `build/final-flow-quality-comparison`: all 12 frames have MAE 0 (not a
  GPU-vs-CPU capture comparison or proof for every possible scene).
- `tests/live_nr_smoke.ps1` is a manual hardware-only background capture test
  requiring the private runtime. It checks actual NR corrections, optional
  GPU capture and normal shutdown using isolated settings. Its hidden window
  means it cannot establish physical display FPS or visual quality.
  Both 15-second NV12 1080p60 runs passed with actual NR active: CPU capture
  855 frames / 823 corrections; GPU capture 840 GPU frames / 0 CPU frames /
  822 corrections. Both exited normally. Startup is included in the duration;
  these counts are not a steady-state FPS comparison.

Still requiring hardware/user review: sustained multi-hour play, 1440p/4K live
GPU-capture color/temporal quality, non-BT.709 sources, unplug/resume and other
GPU/driver combinations. No new GitHub release was created for this batch.

## Expanded GPU-native capture validation

The physical Live Gamer Ultra 2.1 mode list was checked with the manual
`dlss-nr-capture-modes` probe. High-resolution hardware cases explicitly select
the device name as well as the mode; the test fails if the actual saved mode
differs. The first attempt omitted the device name and opened default 1080p60;
those six attempts were rejected, not counted as high-resolution passes.

Final background hardware checks with GPU flow and actual NR enabled:

| Format/mode | Duration | GPU/CPU capture frames | NR corrections | Result |
|---|---:|---:|---:|---|
| NV12 2560x1440@60 | 30s | 1755/0 | 1727 | Passed |
| NV12 3840x2160@60 | 30s | 1740/0 | 1718 | Passed |
| P010 2560x1440@60 | 30s | 1740/0 | 1714 | Passed |
| P010 3840x2160@30 | 30s | 870/0 | 859 | Passed |
| RGB24 1920x1080@60 | 15s | 0/855 | 831 | CPU fallback passed |
| MJPG 1920x1080@60 | 15s | 0/855 | 833 | CPU fallback passed |

All six selected the requested mode and exited normally. Startup is included;
these counts are not steady-state FPS or console-to-display latency. The
hidden-window test checks processing/output counters, not physical display
quality or a moving-game workload. No visual verdict for the live source was
made, and transient incidents between samples are not fully ruled out.

The expanded converter regression exercises 36 changing color-bar frames
across 1080p, 1440p, 4K and a resize back to 720p, with BGRA/NV12/P010. Maximum
RGB channel difference from the CPU SDR BT.709 reference was 2/255 (limit 3).
Four-pixel strips at bar boundaries are excluded due to chroma resampling.
This complements, not replaces, the existing lease/pool/array-slice checks.
It does not validate arbitrary HDR/colorimetry or subjective temporal quality.

Only test tools and documentation were changed for this validation; GPU-native
capture remains opt-in. For the proposed longer automation, see
[long-play validation review](long-play-validation.md).

## Completed follow-up 1-5 (2026-09-05)

1. **Flow recovery/default:** new builds default to GPU handoff. Actual NR
   synthetic probes exercised normal sharing plus create/copy/timeout faults,
   three unload/reload cycles each, including reset and temporal-mode changes.
   All 12 cycles exited normally. A separate CPU-build reference and four GPU
   runs compared 24 aligned game frames at 2560x1440: MAE 0 for every path.
   Copy/timeout probes inject the error return at the eighth copy; they do not
   hang the physical GPU and cannot establish driver-hang recovery. The
   reusable `tests/flow_recovery.ps1` also passed its four synthetic runs with
   exact output and clean shutdown; it accepts an optional local `-InputVideo`.
2. **Capture replay quality:** the supplied 2560x1440@60 game video was decoded
   once per run through the same RGB32 reader and processed via CPU analysis
   or the native BGRA converter/small-analysis path. Both runs completed 1200
   frames with no failures. Stored 320x180 comparisons: MAE 0, zero flagged
   frames. A further 24-frame full-resolution comparison also had MAE 0.
   This validates this fixture's conversion/analysis/NR output, not native YUV
   decoder colorimetry, HDMI timing, all scenes or subjective visual quality.
   NV12/P010 color conversion remains covered by the separate SDR color bars.
   Reports: ignored `build/verify-capture-compare-v2` and
   `build/verify-capture-full-compare-v2`; these quality runs are not timing gates.
3. **Capture transitions:** `tests/live_nr_smoke.ps1 -Transitions -GpuCapture`
   passed in one isolated app with the Live Gamer Ultra 2.1: 30/60 FPS,
   1080p/1440p, P010/RGB24/MJPG/NV12, RGB24 automatic flip and manual flip,
   history overlay, native-capture OFF/ON and injected capture-error reconnect.
   Each step requires new delivered frames, expected saved settings/path and
   active NR correction within 20 seconds. Final reconnect returned to NV12
   1080p60, GPU native, flip OFF and exited normally (2234 GPU/703 CPU frames).
   Physical unplug/replug or sleep/resume is not part of this test.
4. **Diagnostics:** the title reports the last delivered capture path, CPU
   fallback reason and flow handoff path. Copy diagnostics includes the flow
   mode and original sharing HRESULT. Protocol 8 / adapter ABI 5 and the
   versioned bridge timing export reject stale mixed binaries.
5. **Verification/handoff:** local GPU-flow ON and OFF builds each passed all
   eight regression suites, with no hardware skips. Commits and CI are verified separately at
   handoff; no release tag is part of this batch. Private runtime binaries and
   generated comparison outputs are excluded from commits/public packaging.
   Default `build/Release` and `build/gpu-flow-power` binaries were updated with
   hash verification; each private NR runtime hash was preserved. Public ZIP
   whitelist/preservation verification passed without uploading a release.

Long-play automation remains explicitly on hold. GPU-native capture remains
opt-in; passing local checks does not remove the colorimetry/driver limitations.

### CI-discovered shared-copy completion fix

The first follow-up CI run (33942456906) failed the new worker pixel pairing
and video converter tests. D3D device creation alone was insufficient to prove
video-processor support on that runner. The converter test now independently
checks video-processor creation and reports an explicit unsupported skip before
testing conversion; a conversion failure after that gate is still a failure.

The worker failure was not skipped. An explicit WARP worker mode reproduced
sequence 1 metadata with sequence 2 pixels locally. Merely flushing commands
did not resolve it. Shared input/output accesses now wait on a reusable event
query before releasing keyed-mutex ownership, with a one-second upper bound.
This prevents pending copies from observing a later overwrite. Query timeout
fails the operation; the worker exits into existing restart protection.
There is a CPU synchronization cost; this is not a claim of zero-copy or a new
performance improvement. The ninth, mandatory WARP pipeline suite now verifies
all 80 paired frames, output backpressure, resumption and invalid-frame rejection.

Microsoft's [shared-resource guidance](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-opensharedresource)
requires submitting shared-resource updates; the extra completion wait here is
based on the reproduced WARP race, not an assumption that Flush waits for GPU completion.
