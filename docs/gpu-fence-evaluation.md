# GPU input handoff evaluation

Local evaluation: RTX 5090, Style 1, Preset 3, intensity 100, temporal enabled.
Each run uses 30 warmup frames and 120 measured frames. Gaming input is the
Git LFS fixture at 2560x1440/60, starting at the same timestamp.

| Run order | Input-copy host time mean (us) | NR process wall mean (us) | p95 (us) |
| --- | ---: | ---: | ---: |
| Baseline 1 | 142.9 | 6315.0 | 7038 |
| Fence 1 | 60.5 | 6970.9 | 8098 |
| Fence 2 | 58.2 | 6949.1 | 7732 |
| Baseline 2 | 160.7 | 6921.6 | 7947 |

720p synthetic means were 3458.2 us (baseline) and 3459.9 us (fence).
All six NR runs completed their requested measurements with zero processing
failures. All needed the already-existing bounded post-report forced cleanup
because temporal runtime shutdown did not return.

The shared fence removes CPU polling from this handoff, but the GPU still waits:
input_us now represents submission rather than completion. Work can shift into
the later execute_us measurement. These short runs do not establish a total NR
speedup; they are not live FPS, whole-system latency or visual quality results.
Gaming decode time was 14.5-16.4 ms in this offline path, not a measurement of
Media Foundation live capture throughput.

Decision: retain bounded-query input synchronization by default. The alternative
can be built with -DDLSS_NR_EXPERIMENTAL_GPU_FENCE_INPUT=ON in a separate build.
The fence path is single-slot: the caller must finish all D3D12 work before
another copy or destruction. Unsupported fence interfaces fall back to the
query path; signal/wait failure poisons the slot rather than risking reuse.
This does not implement asynchronous multi-buffering or GPU-native MF capture.

Regression coverage alternates fence/query paths for 16 frames at each of two
sizes (including an odd width), verifying exact pixels and frame freshness.
WARP tests require the shared fence path to be exercised, not silently skipped.

Raw local results (not committed) are in build/gpu-fence-* folders. The
candidate-runtime folder is isolated from the normal executable deployment and
contains the user-supplied runtime for local testing only; never redistribute it.
