# Optional two-pass NR

## Three-pass extension

The NR passes menu now also offers **3 passes (experimental)**. The default
remains 1. Saved settings, worker payload and benchmark CLI accept 1–3; values
outside the supported range are rejected by the benchmark/bridge. Selecting a
pass count does not enable NR when it is off. Changes restart the worker.

Three independent NGX feature handles own separate temporal histories. Source
optical flow is generated once and shared across all passes. Additional passes
execute serially, waiting for GPU completion before reusing the FP16 scratch
input/output textures and command state. Each result is copied to the chain
output before the next pass; original source color remains intact for the final
composition. Explicit scratch texture cost stays at the two-pass level, but
the third feature's opaque runtime/history allocations consume additional VRAM.
All extra handles are released before the shared scratch textures. An evaluation
failure reports the failing pass and follows the existing original-video fallback.

This is repeated inference, not frame generation or guaranteed quality gain.
Source flow cannot represent shapes changed by preceding passes. HUD changes,
ghosting, flicker and over-processing may accumulate. No long-session or visual
acceptance is implied by a successful benchmark.

Example: `./tools/benchmark.ps1 -InputVideo 'tests/gaming test sample vd.mp4' -NrPasses 3 -NrScale 100 -GpuCapture`

The historical two-pass measurements below retain their original scope.

Three-pass validation (2026-09-06): RTX 5090, supplied 1440p60 clip,
100% NR size, GPU BGRA replay, 120 warmup + 300 measured frames:
mean 14.119 ms, p95 14.399 ms, p99 14.585 ms, max 25.392 ms;
1/300 frames exceeded 16.67 ms, zero processing failures. Offline decode and
processing together ran at 32.472 fps; neither this throughput nor the 70.826 fps
processing capacity is a live capture FPS measurement. Evidence:
`build/three-pass-300`. Full regression passed 11/11; updated three-pass settings
save/restore and CLI tests also passed on rerun.

Select **Neural Rendering > Creative controls (experimental) > NR passes**.
1 pass is the default; 2 passes is opt-in. The choice is saved as `NrPasses`.
Switching restarts the worker, clears held comparison/history, and shows the
standard information overlay. Creative defaults restores 1 pass. F10 still
controls NR ON/OFF; no new shortcut is reserved. Choosing passes while NR is OFF
does not turn it ON. The title/diagnostic report includes the selected count.

## Execution and boundaries

- Downsample, if selected, once before the chain. Both passes use that size.
- Create two distinct NGX feature handles, each with its own temporal history.
- Generate NVOF once from consecutive original model inputs; both features use
  this source motion field. Do not feed consecutive passes into one temporal
  feature or advance optical-flow history twice for one capture frame.
- Pass 1 output is converted on the GPU to a separate FP16 pass 2 input, with
  the calibrated channel ordering and SDR clamp. Pass 2 uses its own FP16 output.
- Perform source-color/highlight residual composition only after the chain,
  against the original input. No intermediate CPU readback is introduced; the
  existing first-frame channel calibration still reads pass 1 once.
- Wait for pass 1 before reusing parameter/command-list state, then execute pass 2.
  Total worker/benchmark processing time includes both passes and completion.
- Resolution/style/temporal changes release both features. Changing pass count
  in the app restarts the worker; the bridge also resets history on count changes.
- On failure, use the existing error/fallback path, not an unreported one-pass result.

The source-derived motion field cannot describe geometry invented by pass 1.
Independent histories prevent cross-pass contamination but do not guarantee
temporal quality. Effects and artifacts can compound. This is repeated inference,
not 2x upscaling or frame generation.

Additional explicit FP16 textures total 16 bytes per model pixel (about 56.25 MiB
at 2560x1440), plus opaque runtime allocations for the second feature. The latter
may be much larger; memory pressure can cause severe non-linear slowdown.

## Controlled memory retest (RTX 5090, 2026-09-05)

Same executable/DLL/input hashes, 2560x1440 @ 60 BGRA replay, 100% NR size,
120 warmup + 300 measured frames per run, alternating 1/2 passes three times.
ComfyUI was idle in both cases. Its `/free` API unloaded models and freed cached
memory; global VRAM usage fell from 27,498 to 2,282 MiB (32,607 MiB total).

| Condition | Passes | Mean process ms (3 runs) | p95 range ms | Over 16.67 ms |
| --- | ---: | --- | --- | ---: |
| Comfy models resident | 1 | 5.686 / 5.637 / 5.574 | 5.793–5.811 | 0 / 900 |
| Comfy models resident | 2 | 100.046 / 100.027 / 99.847 | 100.134–100.695 | 900 / 900 |
| Comfy models unloaded | 1 | 5.504 / 5.531 / 5.498 | 5.717–5.735 | 0 / 900 |
| Comfy models unloaded | 2 | 9.572 / 9.592 / 9.568 | 9.739–9.828 | 1 / 900 |

All six runs per condition exited normally, no processing errors/timeout/forced
cleanup. This is strong evidence of memory contention, NOT a 100 ms intrinsic
two-pass GPU limit. It does not establish which internal residency/paging operation
caused the delay. Process time includes preparation, NR, composition and completion;
offline sequential decode throughput is not live capture FPS.

After unloading, a 15.26-second native NV12 1440p60 / 100% / 2-pass live smoke
ended with correction active, NR 9 ms, 805 corrections, 656 enhanced frames,
840 captured frames, 0 capture drops and 1,451 audio packets. Counts include startup,
so this is not proof of uninterrupted 60 fresh NR outputs/s or long-session quality.
100% is a viable option on this GPU with memory headroom; 75% is a fallback for
contention or additional processing margin, not a mandatory starting setting.

Local evidence: `build/two-pass-retest-20260905-174713-*` and
`build/two-pass-unloaded-20260905-181038-*` (summary, frame timings, hashes and exit manifests).

## Early short results (confounded by memory occupancy)

Short offline tests used the supplied 2560x1440 game clip and BGRA GPU replay:

| Setting | Warmup / measured frames | Mean process ms | p99 ms |
| --- | --- | ---: | ---: |
| 1 pass, 100% | 30 / 60 | 5.213 | 5.416 |
| 2 passes, 100% | 10 / 30 | 110.976 | 122.855 |
| 2 passes, 75% | 30 / 60 | 7.427 | 7.783 |
| 2 passes, 50% | 30 / 60 | 6.133 | 6.533 |

These early results are retained for provenance, not the current performance
recommendation. They had different warmup/windows and substantial memory occupancy.
The controlled unload comparison above supersedes their 100% feasibility conclusion.

Current TOO SLOW thresholds protect playback, not certify 60 unique NR outputs/s.
Display refresh, fresh processed output rate, fallback rate and latency must be
measured separately. No 60 FPS or long-session quality guarantee is made.

## Verification and reproduction

Worker protocol 11 / adapter ABI 7 require deploying app, worker and adapter
together. The proprietary DLL is unchanged. Both GPU-flow builds pass the full
11-suite regression, including saved two-pass selection/restoration, default
reset, versioned export and CLI bounds. The sample adapter tests metadata only;
actual NR checks are separate hardware tests.

The new WARP two-pass failure fixture verifies that the requested pass count reaches
the adapter, three process failures publish an explicit adapter error, and the next
output matches original pixels rather than silently becoming a successful one-pass
result. This fixture does not execute the proprietary second NGX feature.

The new `-TwoPassTransitions` live check passed in 62.38 seconds: three 1/2-pass
cycles, held-pair invalidation, OFF selection without auto-enable, ON recovery,
30/60 FPS, 1080p/1440p, normal shutdown and two-pass restoration after restart.
Worker DXGI local usage/budget was 1,509 / 31,418 MiB after restart, no pressure hint.
The repeated worker restarts dropped several capture frames per transition; these
are not steady-playback drop measurements. Final restarted run had 0/180 capture
drops. Settings were isolated; no system suspend or physical A/V measurement occurred.

Final hardware checks passed: a 61.30-second 1080p60 HDMI-audio run exercised
1/2-pass switching, reduced sizes, defaults and F9/Tab/F8 comparison. The updated
local deployment then passed a 15.25-second NV12 1440p60 run at 75% / 2 passes:
799 NR corrections, active correction, 1,455 audio packets, normal exit. Startup
is included in these counts; they do not certify 60 unique NR frames per second.

Aligned 960x540 quality captures (120 warmup + 180 frames) preserved the old
75% / 1-pass output exactly (MAE 0). Two passes differed from one by mean RGB MAE
6.63681, maximum frame MAE 8.44863; all 180 frames crossed the review threshold.
This confirms a real additional effect, not improved quality. The sampled still
showed stronger terrain/grass detail and shading. Artifacts remain a visual-review
decision. Local reports: `build/two-pass-one-regression-20260905` and
`build/two-pass-output-compare-20260905`. Quality runs exited without forced cleanup.

```powershell
./tools/benchmark.ps1 -InputVideo 'tests/gaming test sample vd.mp4' -NrPasses 2 -NrScale 75 -GpuCapture -OutputDir build/my-two-pass-run
./tests/live_nr_smoke.ps1 -AppPath build/Release/dlss-nr-capture.exe -GpuCapture -DeviceName 'Live Gamer Ultra 2.1-Video' -Width 2560 -Height 1440 -NrPasses 2 -NrScale 75 -Seconds 15
```

The live smoke uses an isolated settings key and does not overwrite normal user
preferences. Physical A/V calibration and long-session validation remain excluded.

## Memory diagnostics

View > Copy diagnostics reports DXGI LOCAL **worker-process** usage and OS budget
in MiB, not total GPU occupancy. Sampling is once per second on the worker's adapter.
Missing/failed queries and samples older than 3 seconds cannot trigger the hint.
At >=90% of the process budget, an existing TOO SLOW fallback displays
`POSSIBLE VRAM PRESSURE / SHOWING ORIGINAL VIDEO` with the error palette. A high
usage sample alone does not disable NR. A missing hint does not exclude global
contention, opaque driver allocations, or another process retaining VRAM.

The 90% threshold is a conservative diagnostic heuristic, not a causal diagnosis.
Windows documents that exceeding the process budget can cause stuttering or
performance penalties: [DXGI memory accounting](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info).
No other application's model is unloaded automatically. Free other workloads'
models only with user permission; no model files are deleted.

## Visual review pack (100% NR, 2026-09-05)

Generated by `tools/compare-two-pass.ps1` from the supplied game clip. Source frames
120–1019 (2–17 seconds, end exclusive), identical timestamps and input RGB samples
across the 1/2-pass pair; 900 measured frames after 120 warmup. Both use default
creative controls, temporal ON, 100% model size (2560x1440), style 1 / preset 3.
Each capture exited normally without errors, timeout or forced cleanup.

Local artifacts in `build/two-pass-review-20260905/`:

| View | Normal / half speed | Scope |
| --- | --- | --- |
| `full-comparison/` | `comparison.mp4` / `half-speed.mp4` | Whole image, 960x540 samples per column |
| `hud-comparison/` | `comparison.mp4` / `half-speed.mp4` | Native 640x360 top-left ROI, no image downsample |

Columns are labeled **Source / 1 pass / 2 passes**. Normal files contain 900 frames
at 60 fps (15 seconds), slow files 1,800 frames (30 seconds). Half-speed repeats
frames; it does not synthesize optical flow or new motion. They are silent lossy
previews; analysis uses the raw RGB archives and provenance hashes. Quality capture
includes readback/file I/O and is explicitly NOT performance-comparable.

Whole-image mean 1/2-pass RGB MAE was 6.80651 (793/900 review flags); top-left ROI
MAE was 5.73784 (554/900 flags). These are change magnitudes, not quality grades or
proof of flicker/ghosting. First and middle preview stills were checked for column
alignment and readable labels. The HUD sample shows darker/desaturated hearts and
icons with two passes. The middle still contains the map menu, useful for inspecting
text/line distortion. Do not equate stronger terrain detail with better fidelity.

Review at normal speed for flicker around camera/menu transitions, then half-speed
for trails and edge instability. Compare HUD color/shape/text against Source, and
terrain detail against both Source and 1 pass. Final motion/quality acceptance remains
with the user; no long-session validation or physical A/V measurement is claimed.

```powershell
./tools/compare-two-pass.ps1 -InputVideo 'tests/gaming test sample vd.mp4' -OutputDir build/my-review -FfmpegPath 'C:/path/to/ffmpeg.exe'
./tests/live_nr_smoke.ps1 -AppPath build/Release/dlss-nr-capture.exe -GpuCapture -DeviceName 'Live Gamer Ultra 2.1-Video' -TwoPassTransitions -Seconds 8
```

Current regression results: GPU-flow ON **11/11, 21.26 s**; OFF **11/11, 22.10 s**.
Memory warning boundaries (unknown, stale, clock reversal, 90%, over budget) are
automated tests. The live run confirmed real memory telemetry; actual GPU memory
exhaustion was not forced to test the warning visually.

Final synchronized deployment to `build/Release` and `build/gpu-flow-power` preserved
the private DLL hash and backed up replaced binaries. A 15.26-second deployed
1440p60 / 100% / 2-pass NV12 HDMI-audio check ended active at NR 10 ms, 804
corrections, 655 enhanced outputs, 0/841 capture drops and 1,452 audio packets;
normal exit. User settings were not overwritten. No commit, push or release was made.
