# A/V calibration readiness and GPU capture default decision

## CI: complete

The workflow for pushed commit `3093a445e4e021f95076eca56bb53b95cd3c6e80`
completed successfully: [build-and-test run 33945659059](https://github.com/lesser-vr/dlss-nr-capture/actions/runs/33945659059).
This result covers that commit, not the subsequent uncommitted calibration changes.

## A/V: software checks complete, physical measurement skipped

The user explicitly excluded physical A/V measurement from remaining work.
The procedure below is retained for reference only, not an outstanding task.

Added `tools/new-av-sync-reference.ps1`. It creates a lossless FFV1/PCM Matroska
reference with a small central white patch for exactly 100 ms and a 10 ms,
1 kHz click every two seconds. It never plays the file. The 30/60 fps references
and event manifests can be regenerated without committing media:

```powershell
./tools/new-av-sync-reference.ps1 -OutputDir ./build/av-reference-30 -Fps 30
./tools/new-av-sync-reference.ps1 -OutputDir ./build/av-reference-60 -Fps 60
```

Flashing content and clicks are present; use comfortable volume and do not play
if sensitive to flashing images. Use a player/source capable of FFV1 and PCM.
The console cannot be assumed to support this file or accept remote playback.

Local ten-second files are in `build/av-reference-30-v2` and
`build/av-reference-60-v2`. Decoding both files detected flash onsets at
2/4/6/8 seconds and audible threshold crossings at 2.000021/4.000021/
6.000021/8.000021 seconds. The approximately 0.021 ms source offset is one
48 kHz sample and is NOT a capture/display/speaker latency measurement.
Ignore the initial event and decoder end-of-stream silence/black markers.

Deterministic production-clock tests cover 30 -> 60 -> 30 fps, app video delays
0/20/50/0 ms, restart, stale observations, invalid presentation timestamps and
the 200 ms limit. Steady-state estimates are within 3 ms in these simulated
cases. The renderer now clears its presentation-success flag at the beginning
of each render so an early return cannot reuse the previous frame's success.
Both GPU-flow ON and OFF local builds pass all nine regression suites.

Optional future calibration procedure (not scheduled):

1. Play the reference through an HDMI source connected to the capture card.
   This requires a compatible source/player and an explicitly coordinated
   connection change if using a PC instead of the console.
2. Record the output display and audible output together with a camera/mic
   (or calibrated photodiode/audio acquisition). A screen/loopback recording
   measures a software boundary, not physical display/speaker latency.
3. Measure `audio onset - visible flash onset`, skip startup and use multiple
   events. Positive means audio is late; negative means audio is early.
4. Compare manual delay zero, automatic delay OFF/ON, NR OFF/ON and capture
   30/60 fps. Record median, spread and recording frame-time uncertainty.
5. Do not tune compensation from simulated tests alone. Positive manual audio
   delay cannot correct already-late audio; reset to zero and investigate
   source/output latency instead.

No reference signal was routed into the current console/card, and no physical
output recording was acquired. Actual A/V offset and post-transition settling
therefore remain unmeasured. Automatic A/V delay remains experimental, OFF by
default; no claim of physical synchronization or clock-drift correction is made.

## GPU-native capture: keep opt-in

Decision: keep the saved option and default OFF. GPU flow is separate and its
default remains ON. Do not silently override an existing user's selection.

Additional exploratory 2560x1440 game replays used 120 warm-up and 300 measured
frames per route, without quality capture. All four completed with zero failures:

| Format | CPU NR mean / p99 ms | GPU NR mean / p99 ms |
|---|---:|---:|
| NV12 | 6.144 / 6.638 | 5.871 / 7.816 |
| P010 | 6.345 / 8.227 | 5.836 / 7.243 |

These are NR-call timings, not capture latency or live FPS. The offline replay
also performs decode and planar fixture encoding. Some local verification work
overlapped these exploratory runs; host load is not controlled. NV12's p99
exceeded the comparator's 10% threshold while P010 passed. Neither comparison
establishes a repeatable performance win or regression for native hardware
capture. Reports remain local under `build/default-*-comparison-v1`.

Existing color/reference tests and bounded injected-recovery tests support
opt-in use, but do not establish all default-enablement gates. Reconsider after:

- Controlled repeated live CPU/GPU comparisons show acceptable latency/tails.
- Physical USB reconnect and Windows sleep/resume preserve video and audio.
- Relevant formats pass visual acceptance on the actual card/driver.

No HDR support, physical recovery certification, release publication or
long-play automation is implied by this decision.

## Sequential repeat measurement

On 2026-09-05, repeated each format in CPU/GPU/GPU/CPU order with no concurrent
build, regression, live capture or other agent-started GPU work. Each run used
the same executable/runtime/input hashes, 2560x1440 fixture, 120 warm-up and
300 measured frames, style 1/preset 3/intensity 100/temporal ON, no output
capture. OS/background activity and GPU clocks were not externally controlled.
All eight runs completed with zero failures and normal process exit.

| Format | Run | Route | NR mean ms | p95 ms | p99 ms |
|---|---:|---|---:|---:|---:|
| NV12 | 1 | CPU | 6.179 | 6.628 | 7.650 |
| NV12 | 2 | GPU | 5.808 | 6.034 | 7.203 |
| NV12 | 3 | GPU | 5.864 | 6.297 | 7.574 |
| NV12 | 4 | CPU | 6.270 | 6.911 | 7.863 |
| P010 | 1 | CPU | 6.281 | 6.591 | 7.458 |
| P010 | 2 | GPU | 6.026 | 6.983 | 7.738 |
| P010 | 3 | GPU | 5.899 | 6.598 | 7.385 |
| P010 | 4 | CPU | 6.317 | 6.358 | 6.730 |

Averaging the two per-route means gives approximately 6.2% lower NR-call time
for NV12 and 5.3% lower for P010. NV12 tails improved in both comparisons;
P010 tails are mixed and do not establish an improvement. The 1/2 and 4/3
comparisons all passed the existing 10% regression tolerance, which is not a
statistical significance test. Two runs per route are a bounded check, not a
long-session result. Local reports: `build/repeat-{NV12,P010}-pair{1,2}-v2`;
raw summaries, frame timings and hash manifests are in the corresponding
`build/repeat-*-v2` run directories.

The full offline pipeline also includes decode, RGB-to-planar fixture creation,
upload and conversion. Its capacity was CPU 23.114–23.350 / GPU 22.469–22.634
fps for NV12, and CPU 20.992–22.161 / GPU 20.379–21.118 fps for P010. These
are not native capture/play FPS and should not be used to claim a latency or
end-to-end throughput win. The benchmark does not measure live capture tails.

Decision remains default OFF: NR-call mean improvements support continued
opt-in evaluation, but cannot replace live latency and physical recovery gates.
No physical A/V measurement is required by the current task; it is skipped at
the user's request and is not reintroduced as outstanding work.
