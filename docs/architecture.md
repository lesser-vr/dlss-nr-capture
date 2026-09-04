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
- The worker publishes encoded NR correction data; the renderer applies it to the newest live frame.
- Corrections are displayed after 15 consecutive NR results at 15 ms or faster and disabled after five consecutive results at 20 ms or slower. This hysteresis absorbs scheduler jitter while keeping sub-realtime output on the original frame; active corrections must remain fresher than 40 ms.
- The worker publishes per-frame input, optical-flow, GPU preparation/execution, and output timings for live bottleneck diagnosis.
- Coarse NVOF vectors are uploaded once and expanded into the full-resolution normalized motion-vector texture by a D3D12 compute shader.
- DLSS readback is converted directly into the encoded BGRA8 correction buffer, combining channel detection, rejection masking, and residual calculation without an intermediate float RGB output.
- After one-time channel-order detection, D3D12 renders corrections directly into a shared BGRA8 target. D3D11 opens that target and performs a synchronized GPU copy, avoiding steady-state CPU output readback and upload.
- BGRA8 input pixels use a compact upload buffer and a D3D12 compute shader converts them directly into the RGBA16F DLSS color texture.
- NVOF GPU-copies the worker''s D3D11 BGRA texture into its registered ping-pong inputs, eliminating the CPU RGB/luma conversion and upload path.

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

## Next milestone acceptance criteria

- Capture remains GPU-native from Media Foundation to D3D12.
- No CPU pixel round-trip in the steady state.
- Forward and backward flow plus global-flow metadata are visualizable.
- Scene cuts reset all temporal hints.
- Processing timeout falls back to the newest unprocessed frame.
- End-to-end latency and dropped frames are visible in an on-screen overlay.
