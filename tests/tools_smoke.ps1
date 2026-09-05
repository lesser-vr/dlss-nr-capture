param([Parameter(Mandatory=$true)][string]$SourceDir)
$ErrorActionPreference = 'Stop'
. "$SourceDir\tools\file-hash.ps1"
$temp = Join-Path ([IO.Path]::GetTempPath()) ('dlss-tools-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
 $source = Join-Path $temp 'source'
 $files = @('dlss-nr-capture.exe','dlss-nr-worker.exe','dlss-nr-adapter-bridge.dll','dlss-nr-adapter-sample.dll',
  'nr-runtime\dlss5nr_bridge.dll','nr-runtime\caller\nvngx.dll_comfy.dll','nr-runtime\nvngx_dlssnr.dll','private.mp4')
 foreach ($file in $files) {
  $path = Join-Path $source $file
  New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
  'test fixture' | Set-Content -LiteralPath $path
 }
 $runtime = Join-Path $source 'nr-runtime\nvngx_dlssnr.dll'
 $before = Get-ArtifactHash $runtime
 $dest = Join-Path $temp 'package'
 & "$SourceDir\tools\package.ps1" -ReleaseDir $source -OutputDir $dest
 if ((Get-ArtifactHash $runtime) -ne $before) { throw 'Packaging modified source runtime' }
 if ((Test-Path -LiteralPath "$dest\nr-runtime\nvngx_dlssnr.dll") -or (Test-Path -LiteralPath "$dest\private.mp4")) { throw 'Packaging included private content' }
 Add-Type -AssemblyName System.IO.Compression.FileSystem
 $zip = [IO.Compression.ZipFile]::OpenRead($dest + '.zip')
 try {
  if (@($zip.Entries | Where-Object { $_.Name -eq 'nvngx_dlssnr.dll' -or $_.Name -like '*.mp4' }).Count) { throw 'Archive contains private content' }
  if (-not @($zip.Entries | Where-Object Name -eq 'manifest.json').Count) { throw 'Archive manifest missing' }
 } finally { $zip.Dispose() }
 $rejected = $false
 try { & "$SourceDir\tools\package.ps1" -ReleaseDir $source -OutputDir $dest } catch { $rejected = $true }
 if (-not $rejected) { throw 'Existing package was overwritten' }
 Write-Host 'package whitelist, source preservation, archive and overwrite checks passed'
 $baseline = Join-Path $temp 'baseline'
 $candidate = Join-Path $temp 'candidate'
 $summary = [ordered]@{schema_version=1;status='complete';failures=0;performance_comparable=$true;capture_output=$false;
  measured_frames=1;width=1280;height=720;fps_numerator=60;fps_denominator=1;style=1;preset=3;intensity=100;
  temporal=1;warmup_requested=0;adapter_name='test';gpu='test';input='synthetic-v1';
  process_mean_us=1000;process_p95_us=1200;process_p99_us=1300}
 foreach ($dir in @($baseline,$candidate)) {
  New-Item -ItemType Directory -Path $dir | Out-Null
  $summary | ConvertTo-Json | Set-Content -LiteralPath "$dir\summary.json"
  '{"timed_out":false,"sha256":{}}' | Set-Content -LiteralPath "$dir\manifest.json"
  @('frame,timestamp_100ns,warmup,success','0,0,0,1') | Set-Content -LiteralPath "$dir\frames.csv"
 }
 & "$SourceDir\tools\compare-performance.ps1" -Baseline $baseline -Candidate $candidate -OutputDir "$temp\perf-pass"
 $summary.process_mean_us = 1400
 $summary | ConvertTo-Json | Set-Content -LiteralPath "$candidate\summary.json"
 $rejected = $false
 try { & "$SourceDir\tools\compare-performance.ps1" -Baseline $baseline -Candidate $candidate -OutputDir "$temp\perf-fail" }
 catch { $rejected = $true }
 if (-not $rejected -or (Get-Content "$temp\perf-fail\comparison.json" -Raw | ConvertFrom-Json).performance_pass) {
  throw 'Performance regression was not detected'
 }
 $summary.process_timing_boundary = 'prepare-nr-compose-completion-v1'
 $summary | ConvertTo-Json | Set-Content -LiteralPath "$candidate\summary.json"
 $rejected = $false
 try { & "$SourceDir\tools\compare-performance.ps1" -Baseline $baseline -Candidate $candidate -OutputDir "$temp\perf-boundary" }
 catch { $rejected = $true }
 if (-not $rejected -or (Test-Path "$temp\perf-boundary")) { throw 'Different timing boundaries were accepted' }
 $summary.Remove('process_timing_boundary')
 $summary.gpu = 'different'
 $summary | ConvertTo-Json | Set-Content -LiteralPath "$candidate\summary.json"
 $rejected = $false
 try { & "$SourceDir\tools\compare-performance.ps1" -Baseline $baseline -Candidate $candidate -OutputDir "$temp\perf-invalid" }
 catch { $rejected = $true }
 if (-not $rejected -or (Test-Path "$temp\perf-invalid")) { throw 'Invalid performance metadata was accepted' }
 Write-Host 'performance pass, regression gate and incompatible metadata checks passed'
} finally {
 if ([IO.Path]::GetFullPath($temp).StartsWith([IO.Path]::GetTempPath(), [StringComparison]::OrdinalIgnoreCase)) {
  Remove-Item -LiteralPath $temp -Recurse -Force
 }
}
