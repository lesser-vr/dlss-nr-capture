param([Parameter(Mandatory=$true)][string]$ReleaseDir)
$required = @(
  'dlss-nr-capture.exe', 'dlss-nr-worker.exe', 'dlss-nr-adapter-bridge.dll',
  'dlss-nr-adapter-sample.dll', 'nr-runtime\dlss5nr_bridge.dll',
  'nr-runtime\caller\nvngx.dll_comfy.dll'
)
$missing = @($required | Where-Object {
  -not (Test-Path -LiteralPath (Join-Path $ReleaseDir $_) -PathType Leaf)
})
if ($missing.Count) { throw "Missing build artifacts: $($missing -join ', ')" }
$proprietary = Get-ChildItem -LiteralPath $ReleaseDir -Recurse -File | Where-Object {
  $_.Name -in @('_nvngx.dll','nvngx_dlssnr.dll')
}
if ($proprietary) { throw "Proprietary NVIDIA binary found in build output: $($proprietary.FullName -join ', ')" }
Write-Host 'artifact layout checks passed'
