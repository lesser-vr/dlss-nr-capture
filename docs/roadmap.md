# Ordered implementation plan

User-requested order: GPU optimization, then items 1 through 6.

- GPU optimization phase 1 evaluated: shared GPU fence input handoff passes exact-pixel tests, but local NR measurements do not establish an end-to-end speedup. Kept behind DLSS_NR_EXPERIMENTAL_GPU_FENCE_INPUT (default OFF). See gpu-fence-evaluation.md. GPU-native capture and multi-buffering remain deferred pending live-path profiling; this is not a completed zero-copy capture pipeline.
- Complete 1: Video endpoint identity and missing-device startup preference preservation; restart regression passed.
- Complete 2: Serialized MTA driver calls service window messages; commands are blocked while busy. Closing during a stalled call terminates this app rather than detaching threads with dangling references. Injected stall/close regression passed.
- Complete 3: Bounded 200ms output backlog and positive manual audio delay (0/25/50/100/200ms), bounded delay queue, saved settings and unit coverage. Not automatic timestamp-locked A/V sync.
- Complete 4: Bounded local error/recovery event log with a previous segment, nonfatal write failures and rotation tests.
- Complete 5: Whitelisted ZIP packaging, SHA-256 manifest, source/private runtime preservation and overwrite tests, CI packaging step and documentation reconciliation.
- Complete 6: Timing baseline gate (mean/p95/p99), full-resolution and ROI RGB output, metadata alignment and automatic tests. Actual NR 2560x1440 full-frame and 640x360 ROI saves passed. Visual quality remains a review decision.

Actual USB/HDMI reconnection, sleep/resume, long-play audio drift and visual NR quality remain hardware validation tasks. OBS output is optional and outside this ordered implementation batch.

Final validation: all six regression suites passed. Public ZIP packaging was
executed against the deployed build; proprietary runtime SHA-256 was unchanged.
Actual NR file-capture checks completed with no processing failures, but still
needed the existing bounded post-report cleanup for stalled runtime shutdown.

## Post-beta follow-up

Update 2026-09-05: GPU-native capture is now an opt-in saved menu option and
the worker uses bounded input/processing/output buffers with paired metadata.
The earlier deferred statements above describe the prior batch, not the current
implementation. Flow-default decision, nine-suite coverage and limitations:
[GPU capture pipeline](gpu-capture-pipeline.md).

- NR shutdown: paired snippet shutdown and explicit global GPU resource cleanup
  added; repeated actual NR runs now exit without forced cleanup.
- GPU optimization phase 2: coarse-flow handoff measured, CPU-transfer recovery
  implemented and fault-tested. Default ON for new CMake builds; this is separate
  from the still-opt-in GPU-native capture path.
- Long-session blackout: not reproduced; added sparse source/output probes and
  presentation-error logging to distinguish candidate causes without hiding frames.
- Details and measured results: [investigation](shutdown-gpu-blackout.md).
- Follow-up priorities 1–5: event-based shared-copy completion, SDR metadata,
  graphics/resume recovery, GPU vertical flip and experimental A/V delay are
  implemented. Bounded live transitions and planar game replay were tested;
  physical unplug/sleep remain acceptance work; physical A/V measurement was
  subsequently skipped at the user's request.
  Small synchronous analysis readback is retained pending evidence that an
  asynchronous path is worth its alignment/latency tradeoff.
  See [results and limitations](capture-stability-sync.md).
- Latest pushed CI passed. A/V reference generation and software transition
  tests are ready; physical input/output calibration is excluded at the user's
  request. GPU-native capture remains opt-in after the default
  review. See [calibration readiness and default decision](av-calibration-gpu-default.md).
- Experimental creative controls now expose Tone/Structure, 75/50% NR processing,
  residual color/highlight protection and held-frame comparison with centered
  zoom. Both GPU-flow builds passed all 10 regression suites, including 15
  numerical composition cases; short hardware NR/UI transitions passed. Subjective
  quality and sustained performance remain unvalidated. See
  [implementation notes](nr-creative-controls.md) and [test policy](regression-policy.md).
