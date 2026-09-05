# Experimental NR creative controls

Implemented after the DLSS 5 upstream review; functional regression now covers
composition and hardware NR/UI transitions. Subjective quality is not certified. Defaults
remain Tone 100%, Structure 100%, NR resolution 100%, source-color preservation
0%, highlight protection OFF. GPU-native capture defaults are unchanged.

## Controls

Neural Rendering > Creative controls (experimental):

- Tone and Structure: independent 0/25/50/75/100% values forwarded to the existing
  runtime parameters. The UI uses a conservative 0–1 range; exact artistic
  effects depend on the private runtime and model. These are separate from the
  existing overall Intensity option.
- NR resolution: 100/75/50% of each input dimension, rounded down to even sizes
  below 100%. This does not change capture format, frame rate or display size.
- Preserve source color: 0/25/50/75/100%. Progressively remove chromatic parts
  of the model residual while retaining its Rec.709-weighted brightness delta.
  This is an SDR encoded-RGB approximation, not hue-perfect color management.
- Protect highlights: progressively attenuate the residual in bright original
  pixels, using a smooth transition from maximum RGB 0.75 to 1.0. This preserves
  source highlights rather than reconstructing information already clipped by
  the console/card. It is not HDR tone mapping or the upstream reversible proxy.
- Restore creative defaults resets only these five controls. They are saved
  with the existing settings. Changing them restarts NR and releases frame hold
  so output from the old configuration cannot be presented as the new one.

## Reduced-resolution processing

The worker keeps its full-resolution input/output contract. A worker-private
D3D11 pass downsamples the source with area-overlap weights (not a single point
sample), preserving a copy of that exact small input. NR and NVOF run on the
small texture; the bridge's MV resource sizes/scales are consequently derived
from the same small dimensions. The existing analysis policy still controls
history resets; its pixel motion is not supplied as the model's flow field.

The output is composed as full original + bilinearly reconstructed
(small NR output - matching small input). Optional color/highlight attenuation
is applied to that delta before SDR clamping. This avoids replacing the full
image with a blurred enlargement, but may produce residual halos and cannot
preserve all generated fine detail. The implementation is independent code,
not a wholesale import of OptiScaler or RenoDX shader code.

At 100% with both composition controls disabled the extra composition resources
and passes are bypassed. Otherwise extra GPU textures and passes are required.
No specific speedup or quality improvement is established by compilation.
Published worker processing time now includes preparation/composition and their
completion wait; NR-only adapter substage timings remain separately available.
Older benchmark numbers are not directly comparable with this timing boundary.

## Held comparison

F8 holds an already matched original/NR pair in F9 comparison mode. It waits for
a valid pair rather than labeling unrelated input as a comparison. Existing
buffers are retained; new model submissions pause, while capture and audio
continue. F8 resumes. Tab still shows the held original, and the menu cycles
centered 1x/2x/4x zoom. This is a display freeze, not a scene replay: changing
settings releases hold instead of evaluating the same frame with new settings.
Held images are excluded from new-frame A/V observations. Comparison consumes
its previously bounded history budget; creative processing has separate bounded
worker-private textures.

## Compatibility and verification

Measured results and conservative recommendations:
[2026-09-05 evaluation](creative-evaluation-20260905.md).

`tools/evaluate-creative.ps1 -InputVideo PATH -OutputDir NEW_DIRECTORY` runs five
conditions (100/75/50% NR, 100% source-color preservation, highlight protection)
with three rotating timing repeats, then aligned 960x540 quality samples and a
repeated baseline to measure runtime variability. Tone/Structure stay at 100%.
Use a video long enough for warmup plus measured frames (defaults: 120+300).
Raw video artifacts stay local; do not commit the private runtime or generated
archives. This is not an automated subjective-quality pass.

`tools/benchmark.ps1` also accepts `-NrScale`, `-Tone`, `-Structure`,
`-ColorPreserve`, and `-HighlightGuard`. It uses the same `NrComposition` helper
as the worker and initializes NR at the actual reduced dimensions. The
`process_timing_boundary` field identifies preparation + NR + composition + GPU
completion wall time. Do not compare these timings with older adapter-only runs.
Quality capture is a separate, non-performance-comparable run. Decoding and
analysis are included in pipeline capacity, but live capture/presentation are not.

Worker protocol is 9 and adapter ABI is 6 because the parameter payload changed.
Deploy app, worker and adapter binaries together. The proprietary NR DLL is not
modified or redistributed. Public bridge exports remain unchanged.

On 2026-09-05 the full suite passed 10/10 in both GPU-flow ON and OFF builds.
The new WARP composition test passed 15 numerical cases for area downsampling,
identity residuals, bypass and color/highlight composition. An actual RTX 5090
capture run passed reduced-size NR initialization, creative defaults, F9/Tab/F8,
held zoom/swap, focus-loss release and settings-change recovery (45.89 seconds).
A second combined run with HDMI audio passed in 107.31 seconds and exited normally:
30/60 FPS, 1080p/1440p, NV12/P010/RGB24/MJPG, GPU/CPU fallback, injected capture
error, graphics recreation, application-only resume and all creative transitions.
Audio packets continued and the final state had active NR. This did not suspend
Windows or physically disconnect the card, and reported latency is not a physical
A/V measurement. Transition-related frame drops were not a zero-drop performance gate.
These are numerical and state-transition checks, not visual quality certification
or sustained performance measurements. See the [verification policy](regression-policy.md).
Keep the new controls experimental; use Restore creative defaults if artifacts
appear. A/V physical measurement remains excluded at the user's request.
