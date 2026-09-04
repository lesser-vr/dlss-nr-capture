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
