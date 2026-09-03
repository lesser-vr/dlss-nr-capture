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
- GPU processing will move to a dedicated worker with explicit fences.

This intentionally prefers a dropped frame over accumulated input latency.

## Processing boundary

`IFrameProcessor` is deliberately runtime-agnostic. Future implementations are:

1. `PassthroughProcessor` — safe fallback and pipeline validation.
2. `MotionAnalysisProcessor` — optical flow, confidence, cut detection.
3. `NrWorkerProcessor` — shared-texture IPC to an isolated D3D12/NGX worker.

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
