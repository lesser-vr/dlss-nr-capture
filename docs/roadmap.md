# Ordered implementation plan

User-requested order: GPU optimization, then items 1 through 6.

- GPU optimization phase 1 evaluated: shared GPU fence input handoff passes exact-pixel tests, but local NR measurements do not establish an end-to-end speedup. Kept behind DLSS_NR_EXPERIMENTAL_GPU_FENCE_INPUT (default OFF). See gpu-fence-evaluation.md. GPU-native capture and multi-buffering remain deferred pending live-path profiling; this is not a completed zero-copy capture pipeline.
- Complete 1: Video endpoint identity and missing-device startup preference preservation; restart regression passed.
- Implemented 2: Serialized MTA driver calls service window messages; commands are blocked while busy. Closing during a stalled call terminates this app rather than detaching threads with dangling references. Validation in progress.
- Pending 3: Bound audio playback backlog and add audio/video latency control.
- Pending 4: Persistent diagnostic logs and recovery history.
- Pending 5: Automated distribution packaging, proprietary runtime preservation checks, and documentation reconciliation.
- Pending 6: Automated performance baseline comparison and higher-resolution/ROI quality review.

Actual USB/HDMI reconnection, sleep/resume, long-play audio drift and visual NR quality remain hardware validation tasks. OBS output is optional and outside this ordered implementation batch.
