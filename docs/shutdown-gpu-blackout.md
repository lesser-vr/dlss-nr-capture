# Shutdown, GPU flow, and blackout investigation

Historical investigation below. The subsequent default-ON flow decision, CPU
handoff recovery and eight-suite coverage are recorded in
[GPU capture pipeline](gpu-capture-pipeline.md). Earlier default/verification
statements in this investigation describe their original revision.

## Shutdown

The local reproduction stalled at FreeLibrary during teardown, after feature,
NVOF, and NGX core cleanup. The snippet NR module had been initialized through
DLSSNR_CallInit but its matching DLSSNR_CallShutdown was never called. The
existing caller shim already exported that function. The bridge now requires
both shutdown exports, tracks which sessions initialized successfully, shuts
down NR before the core, and clears the missing global motion-vector pipeline
and root signature before DLL unloading.

After the change, three fresh-process cycles of temporal OFF and ON (six total)
completed without forced cleanup, as did four 1440p timing runs and both quality
captures. Run tools/check-nr-shutdown.ps1 with a runtime-equipped ReleaseDir and
a new OutputDir to repeat this check. The proprietary runtime is never shipped.
The existing benchmark timeout remains a safety net for other driver/runtime
failures, not the normal shutdown path.

Final verification: all six regression suites passed in both the default and
GPU-flow builds. The actual-NR probe also completed three unload/reload cycles
inside the same process for each build, including history resets and temporal
mode changes. Updated default binaries were copied to build/Release with hash
verification; the user's proprietary runtime was unchanged. No release or push
was performed for these changes.

## GPU-only coarse optical flow (experimental)

Configure a separate build with -DDLSS_NR_EXPERIMENTAL_GPU_FLOW=ON.
NVOF exposes its borrowed D3D11 flow texture; a bounded synchronized copy into
a D3D12-owned shared texture replaces staging Map, CPU vector copy, and upload.
The compute shader reads signed S10.5 values directly. First/reset frames still
write zero motion vectors. The slot is reused only after D3D12 completion.

Cross-API sharing uses R16G16_TYPELESS with ALLOW_SIMULTANEOUS_ACCESS and a
R16G16_SINT shader view. Sharing the typed SINT texture directly failed locally.
See Microsoft's [cross-API sharing tiers](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_shared_resource_compatibility_tier).
WARP tests verify exact 32-bit payloads across repeated frames, odd dimensions,
and both bounded-query and shared-fence handoffs.

RTX 5090, supplied gaming video, 2560x1440/60, style 1, preset 3, intensity 100,
temporal ON; each timing run has 30 warmup and 120 measured frames:

| Order | NR process mean (us) | p95 (us) | p99 (us) |
| --- | ---: | ---: | ---: |
| Baseline 1 | 6724.642 | 8291 | 9164 |
| GPU flow 1 | 6451.992 | 7989 | 8267 |
| GPU flow 2 | 6105.842 | 7340 | 7670 |
| Baseline 2 | 6998.750 | 9166 | 9811 |

All runs had zero processing failures and clean exit. Six full-resolution
1440p frames after 30 warmup frames compared byte-for-byte equal (RGB MAE 0).
This is a small sample, not a general quality guarantee. These are offline NR
processing times, not capture-to-display latency or playable FPS. Other GPU
activity was not controlled, so repeat measurements before promotion.

The option stays OFF by default pending longer hardware/driver coverage.
GPU-native Media Foundation capture and asynchronous multi-buffering remain
unimplemented; this change removes only the coarse-flow CPU round trip.

## Intermittent black screen

At the start of investigation the running app came from build-vs2026-async,
not the current release/build; the current event-log file was absent. The
reported long-session blackout has not been reproduced, and no cause is
confirmed. No recent Display-provider reset event was returned by the local
System-log query; this does not rule out device loss.

Present results were previously ignored. New diagnostics log non-busy result
changes and GetDeviceRemovedReason. A once-per-second asynchronous nine-point
probe compares the current source with the rendered video before overlays;
classification changes and one-minute heartbeats go to capture.log.
No video/audio pixels are saved. The staging resource contains only nine
pixels, read with DO_NOT_WAIT; it never adds a blocking readback.

- source_dark=1 and output_dark=1: source-side darkness or a normal game fade.
- source_dark=0 and output_dark=1 with nr_active=1: inspect NR/history/display
  composition, while allowing for the normal temporal offset between frames.
- Non-success Present/device-removed codes: inspect presentation/driver reset.
- CAPTURE INTERRUPTED/recovery entries: inspect capture connection or driver.

Sparse samples can miss short/partial blackouts and dark scenes can resemble
blackouts. The probe observes a rendered texture, not the physical screen.
HDMI/HDCP renegotiation and display-side problems need hardware reproduction.
Diagnostics do not suppress dark frames or alter NR/fallback thresholds.
Use the updated app and record the time, audio continuity, whether menus and
overlays remain visible, and whether F10 restores the picture when it recurs.

The capture-reconnect smoke test initially timed out twice, then passed alone.
It uses the physical device when available and is timing/environment sensitive;
the failure message now preserves the final window title for investigation.
