param(
  [Parameter(Mandatory=$true)][string]$ReleaseDir,
  [Parameter(Mandatory=$true)][string]$SourceDir
)
$required = @(
  'dlss-nr-capture.exe', 'dlss-nr-worker.exe', 'dlss-nr-adapter-bridge.dll',
  'dlss-nr-adapter-sample.dll', 'nr-runtime\dlss5nr_bridge.dll',
  'nr-runtime\caller\nvngx.dll_comfy.dll'
)
$missing = @($required | Where-Object {
  -not (Test-Path -LiteralPath (Join-Path $ReleaseDir $_) -PathType Leaf)
})
if ($missing.Count) { throw "Missing build artifacts: $($missing -join ', ')" }
$tracked = @(git -C $SourceDir ls-files -- '*nvngx_dlssnr.dll' '*_nvngx.dll')
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect tracked repository files' }
if ($tracked.Count) { throw "Proprietary NVIDIA binary is tracked by Git: $($tracked -join ', ')" }
Write-Host 'artifact layout checks passed'
