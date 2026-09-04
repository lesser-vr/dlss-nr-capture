# Architecture

## Product shape

The primary product is a standalone, low-latency viewer. OBS is an optional
consumer of the processed shared texture, not the owner of the processing
pipeline.

## Thread and queue policy

- Media Foundation invokes capture callbacks asynchronously.
- Capture publishes at most one pending frame.
- A newly captured frame replaces an unrendered older frame.
- Rendering runs on the window thread for this milestone.
- GPU processing runs in a dedicated worker with separate keyed-mutex input and output textures.
- The worker publishes the completed NR frame and the renderer displays that temporally aligned result directly instead of adding an older residual to the newest live frame.
- NR speed thresholds follow the selected rational capture FPS: activate below 90% of the frame budget after ceil(FPS/2) consecutive qualifying EMA samples; latch slow above 150% after ceil(FPS) slow samples. These are result counts, not wall-clock deadlines. At 60 FPS the tested 15/25 ms and 30/60 samples are unchanged. Invalid rates fall back to 60 FPS, and mode changes reset EMA/counters. During brief scheduling gaps the last completed NR frame may be repeated for up to 100 ms instead of flashing back to the live original.
- The worker publishes per-frame input, optical-flow, GPU preparation/execution, and output timings for live bottleneck diagnosis.
- Coarse NVOF vectors are uploaded once and expanded into the full-resolution normalized motion-vector texture by a D3D12 compute shader.
- DLSS readback is converted directly into the final BGRA8 output, combining channel detection and rejection masking without an intermediate float RGB output.
- After one-time channel-order detection, D3D12 renders corrections directly into a shared BGRA8 target. D3D11 opens that target and performs a synchronized GPU copy, avoiding steady-state CPU output readback and upload.
- The worker copies BGRA8 input into a D3D12-owned shared texture opened by D3D11. A bounded D3D11 event query confirms copy completion before D3D12 reads it; the compute pass transitions COMMON -> shader resource -> COMMON, and existing D3D12 fence completion precedes slot reuse. The single-slot path is serialized, not an asynchronous multi-buffer pipeline. Failed copy synchronization poisons the slot until recreation.
- A typed BGRA SRV feeds the RGBA16F conversion shader without steady-state CPU input readback or re-upload. The first frame still supplies CPU reference pixels for output channel-order calibration; that staging texture and CPU buffer are released after success. This does not remove CPU work earlier in capture/analysis, or the coarse motion-vector upload.
- NVOF GPU-copies the worker''s D3D11 BGRA texture into its registered ping-pong inputs, eliminating the CPU RGB/luma conversion and upload path.
- NVOF is authoritative for NR temporal motion. The low-resolution CPU translation model remains diagnostic-only for rejection masks, because applying those masks during camera rotation caused raw/NR oscillation. Hard cuts additionally require a large luminance-histogram change before resetting NR history.
- Large zero-translation luminance changes, such as opening a game menu, trigger one NR history reset followed by a 30-frame cooldown so menu animations cannot repeatedly reset temporal processing.
- History resets preserve the user's temporal mode: NVOF primes a new frame pair and NR receives reset with zero motion vectors, without switching feature modes or recreating frame resources.
- Each worker restart excludes the first two seconds after its first completed output from speed evaluation. The renderer shows NR PREPARING until qualification succeeds or sustained slowness is established; the first post-warmup sample seeds a fresh EMA. Toggle notifications retain display priority.

This intentionally prefers a dropped frame over accumulated input latency.

## Processing boundary

`IFrameProcessor` is deliberately runtime-agnostic. Future implementations are:

1. `PassthroughProcessor` — safe fallback and pipeline validation.
2. `MotionAnalysisProcessor` — optical flow, confidence, cut detection.
3. `NrWorkerProcessor` — dual shared-texture IPC to an isolated D3D12/NGX worker.

The application must never require or download a proprietary runtime merely to
start. A worker crash or unsupported runtime must switch the processor back to
passthrough.

## Motion policy

Camera motion is not blindly subtracted from the final screen-space motion
vector. A global or multi-plane camera model is used to validate and regularize
the observed flow. Forward/backward disagreement, disocclusion, scene cuts and
low-confidence regions reject temporal history.

## Remaining GPU work (not current guarantees)

- Capture remains GPU-native from Media Foundation to D3D12.
- No CPU pixel round-trip in the steady state.
- Forward and backward flow plus global-flow metadata are visualizable.
- Scene cuts reset all temporal hints.
- Processing timeout falls back to the newest unprocessed frame.
- App-local latency and dropped frames are visible; console-to-display latency requires external measurement.

## Driver operations and audio

Driver calls run on MTA threads while a restricted UI message loop handles
window management. User commands/timers cannot reenter device state. Threads
are not detached: an explicit close during a blocked call terminates this app,
and post-window shutdown has a five-second deadline. This is not driver-level
cancellation or automatic isolation of a faulty kernel driver.

Audio uses a bounded delayed-packet queue and a 200ms waveOut backlog ceiling.
Selectable positive audio delay is manual synchronization assistance, not
timestamp-locked A/V playback. See README for settings, local logs and packaging.
