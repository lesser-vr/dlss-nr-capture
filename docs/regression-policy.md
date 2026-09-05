# Regression verification policy

Skipping regression applies only to the requested task, unless the user explicitly
sets a longer scope. Previous skips do not exempt later changes. Select the
highest-risk category touched by a change:

| Change | Required verification |
| --- | --- |
| Text, color, cosmetic UI | Build and targeted UI check |
| Input and comparison UI | Build plus shortcut and state-transition checks |
| NR processing, GPU buffers, worker protocol or adapter ABI | Full regression suite and short actual NR run; validate GPU-flow ON and OFF builds when affected |

A build alone is not a regression pass. Report executed checks, failures and
unverified areas separately. If hardware or the private runtime is unavailable,
report the hardware checks as not run rather than passed. Do not silently hide
failures or extend a skip to unrelated work.

Run the automated suite with:

```powershell
cmake --build build-gpu-flow --config Release --target regression
cmake --build build-vs2026-watchdog --config Release --target regression
```

For an available capture card and user-provided NR runtime, use the isolated
hardware smoke test (no other capture app instance should be running):

```powershell
./tests/live_nr_smoke.ps1 -AppPath ./build-gpu-flow/Release/dlss-nr-capture.exe -GpuCapture -DeviceName 'Live Gamer Ultra 2.1-Video' -Transitions -CreativeTransitions -Seconds 8
```

`regression.nr-composition` checks area downsampling, identity residuals, the
default bypass, color preservation and highlight attenuation against independent
CPU expectations with WARP. Creative transitions exercise actual NR initialization
at 100/75/50 percent and F9/Tab/F8, zoom, swap, focus loss and settings changes.
These checks establish functional behavior, not subjective quality or sustained
gaming performance. Physical A/V measurement remains excluded at the user's
request; long-session testing remains deferred. Physical unplug/suspend tests
require coordination and are not implied by the short smoke test.
