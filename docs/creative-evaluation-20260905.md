# Creative controls evaluation — 2026-09-05

## Method

- RTX 5090; GPU-flow ON build; same local game video, 2560x1440 at 60 FPS.
- Style 1, preset 3, intensity 100, temporal ON, Tone/Structure 100 throughout.
- BGRA GPU-capture replay: this is decoded file replay, not capture-card timing.
- Five independent conditions, three rotating-order timing runs each; 120 warmup
  frames then 300 measured frames (source frames 120–419).
- Process wall time includes downsample, NR, residual composition and GPU completion.
  Disk output is disabled for timing. This boundary differs from older benchmarks.
- Separate quality runs: 180 frames, same warmup, 960x540 nearest-sampled RGB
  from the whole input. A repeated 100% baseline tests runtime variability.
- All 21 runs completed without NR failure, timeout or forced cleanup.

Input SHA-256: `37E53A661272D7DDB043BE7E48DE08DF18C1A92C8D8C72A0D047D70AC639C147`.
Benchmark SHA-256: `828A39E84A888E0ECB786B8545A526C6513E3FE6D40896D441E0BDDE343D51E1`.
Per-run manifests also record adapter/runtime hashes. Proprietary files and raw
video archives are not added by this change.

## Performance

Values are the median of three run-level means / p95 values, not pooled p95.

| Condition | Model size | Mean median (ms) | Mean range (ms) | p95 median (ms) |
| --- | --- | ---: | ---: | ---: |
| NR 100% | 2560x1440 | 5.766 | 5.647–5.772 | 5.931 |
| NR 75% | 1920x1080 | 4.711 | 4.611–4.780 | 4.901 |
| NR 50% | 1280x720 | 3.964 | 3.877–4.018 | 4.217 |
| 100% + color preservation 100% | 2560x1440 | 5.749 | 5.723–6.378 | 6.010 |
| 100% + highlight protection | 2560x1440 | 5.732 | 5.452–5.732 | 5.968 |

75% and 50% reduced this processing time by approximately 18.3% and 31.3%.
Color/highlight differences are within run variability: do not claim they speed
up NR. The third color run had a mean of 6.378 ms and p95 of 8.068 ms; it was
retained, not discarded. No measured process frame exceeded the 16.67 ms budget.

End-to-end offline pipeline capacity remained roughly 37–49 FPS, including
decoding and analysis. NR-only capacity is NOT achievable live/display FPS.
These results do not establish capture-card latency, fallback rate or prolonged
thermal stability and do not replace live play checks.

## Quality observations

Raw input pixels and frame timestamps matched for every comparison. The repeated
baseline was exactly identical (MAE 0), making these observed setting differences
larger than the measured repeat variability for this clip.

| Candidate vs 100% baseline | Mean RGB MAE (0–255) | Largest frame MAE | Review flags / 180 |
| --- | ---: | ---: | ---: |
| 75% | 1.77548 | 3.05491 | 0 |
| 50% | 1.94395 | 3.20628 | 0 |
| Color preservation 100% | 1.63035 | 1.87822 | 0 |
| Highlight protection | 0.08726 | 0.11666 | 0 |

These are differences, not scores of correctness. No flags does not mean no
ghosting/flicker. Metrics are not motion compensated. Inspection of aligned stills
at sample 90 and the large-change sample 160 showed changes in terrain/grass
texture and shading; the reduced-scale output is not equivalent to 100% NR.
Color preservation changes the color treatment; highlight protection has only
a small effect in this scene. No strong broad halo was apparent in these sampled
stills, but fine boundaries and temporal artifacts cannot be cleared by a 960x540
preview or two stills. The source includes its own HUD/overlay and is not a
ground-truth reference. This was a short outdoor scene, not a diverse game corpus.

## Recommendation and local review artifacts

Keep defaults unchanged: NR 100%, color preservation 0%, highlight protection OFF.
Try 75% first when processing headroom is needed; reserve 50% for a stronger
performance/appearance tradeoff. Enable color preservation or highlights only
for a desired visual effect, not as a general performance improvement.
Do not promote the creative controls out of experimental status on this evidence.

Local artifacts: `build/creative-evaluation-20260905/`. Each `video-scale75`,
`video-scale50`, `video-color100`, and `video-highlights` folder contains a report
and a 3-second MP4 with columns **source / 100% baseline / candidate**. MP4 is a
lossy viewing aid; numerical metrics use raw RGB. Contact sheets use rows
source/100%, 75%/50%, color/highlights (sample 90) and the first two rows (160).
Raw reports, CSVs and manifests are retained locally for rechecking.

Verification: both GPU-flow build configurations passed 10/10 suites after the
benchmark integration; final comparison-tool changes passed the tools and
benchmark suites again. No physical A/V measurement or long-session test ran.
