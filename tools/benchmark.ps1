param(
  [string]$InputVideo,
  [switch]$Synthetic,
  [switch]$CaptureOutput,
  [switch]$FullResolution,
  [switch]$GpuCapture,
  [ValidateSet('BGRA','NV12','P010')][string]$CaptureFormat='BGRA',
  [ValidateRange(0,3)][int]$TestFlowFailure = 0,
  [ValidateRange(1,8192)][int]$QualityWidth = 320,
  [ValidateRange(1,8192)][int]$QualityHeight = 180,
  [ValidateRange(0,8191)][int]$RegionX = 0,
  [ValidateRange(0,8191)][int]$RegionY = 0,
  [ValidateRange(0,8192)][int]$RegionWidth = 0,
  [ValidateRange(0,8192)][int]$RegionHeight = 0,
  [string]$ReleaseDir = "$PSScriptRoot\..\build\Release",
  [string]$OutputDir,
  [ValidateRange(1,1000000)][int]$Frames = 300,
  [ValidateRange(0,1000000)][int]$Warmup = 120,
  [ValidateRange(0,3)][int]$Style = 1,
  [ValidateRange(1,4)][int]$Preset = 3,
  [ValidateRange(25,100)][int]$Intensity = 100,
  [ValidateRange(0,1)][int]$Temporal = 1,
  [ValidateSet(50,75,100)][int]$NrScale = 100,
  [ValidateRange(0,100)][int]$Tone = 100,
  [ValidateRange(0,100)][int]$Structure = 100,
  [ValidateRange(0,100)][int]$ColorPreserve = 0,
  [ValidateRange(0,1)][int]$HighlightGuard = 0,
  [ValidateRange(1,86400)][int]$TimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'
function Get-Sha256([string]$Path) {
  $stream = [IO.File]::OpenRead($Path)
  $sha = [Security.Cryptography.SHA256]::Create()
  try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
  finally { $sha.Dispose(); $stream.Dispose() }
}
if ([bool]$InputVideo -eq [bool]$Synthetic) { throw 'Specify exactly one of -InputVideo or -Synthetic' }
$release = (Resolve-Path -LiteralPath $ReleaseDir).Path
if (-not $OutputDir) { $OutputDir = Join-Path $release ('benchmark-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)) }
if (Test-Path -LiteralPath $OutputDir) { throw 'Use a new output directory' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir).TrimEnd('\')
$arguments = @('--output', $OutputDir, '--warmup', $Warmup, '--frames', $Frames,
    '--style', $Style, '--preset', $Preset, '--intensity', $Intensity, '--temporal', $Temporal,
    '--nr-scale',$NrScale,'--tone',$Tone,'--structure',$Structure,'--color-preserve',$ColorPreserve,'--highlight-guard',$HighlightGuard)
if ($CaptureOutput) { $arguments += '--capture-output' }
if ($GpuCapture) { $arguments += '--gpu-capture' }
$arguments += @('--capture-format',$CaptureFormat)
if ($PSBoundParameters.ContainsKey('TestFlowFailure')) { $arguments += @('--test-flow-failure',$TestFlowFailure) }
if (-not $CaptureOutput -and ($FullResolution -or $QualityWidth -ne 320 -or $QualityHeight -ne 180 -or
    $RegionX -or $RegionY -or $RegionWidth -or $RegionHeight)) { throw 'Quality options require -CaptureOutput' }
if ($CaptureOutput) {
 $arguments += @('--quality-width',$QualityWidth,'--quality-height',$QualityHeight,'--quality-x',$RegionX,'--quality-y',$RegionY)
 if ($RegionWidth) { $arguments += @('--quality-region-width',$RegionWidth) }
 if ($RegionHeight) { $arguments += @('--quality-region-height',$RegionHeight) }
 if ($FullResolution) { $arguments += '--full-resolution' }
}
$files = @('dlss-nr-benchmark.exe','dlss-nr-adapter-bridge.dll','nr-runtime\dlss5nr_bridge.dll',
    'nr-runtime\caller\nvngx.dll_comfy.dll','nr-runtime\nvngx_dlssnr.dll')
$hashes = [ordered]@{}
foreach ($file in $files) { $hashes[$file] = Get-Sha256 (Join-Path $release $file) }
if ($Synthetic) { $arguments += '--synthetic' }
else {
  $inputPath = (Resolve-Path -LiteralPath $InputVideo).Path
  $arguments += @('--input', $inputPath)
  $hashes['input_video'] = Get-Sha256 $inputPath
}
$started = [DateTime]::UtcNow.ToString('o')
$quoted = @($arguments | ForEach-Object {
  $value = [string]$_
  if ($value.Contains('"')) { throw 'Unsupported quote in argument' }
  '"' + $value + '"'
}) -join ' '
$process = Start-Process -FilePath (Join-Path $release 'dlss-nr-benchmark.exe') -ArgumentList $quoted -WindowStyle Hidden -PassThru
$clock = [Diagnostics.Stopwatch]::StartNew()
$ready = $null; $forced = $false; $timedOut = $false
try {
  while (-not $process.HasExited) {
    if ($null -eq $ready -and (Test-Path -LiteralPath (Join-Path $OutputDir 'summary.json'))) {
      try {
        $null = Get-Content -LiteralPath (Join-Path $OutputDir 'summary.json') -Raw | ConvertFrom-Json
        $ready = $clock.Elapsed.TotalSeconds
      } catch { }
    }
    if ($null -ne $ready -and $clock.Elapsed.TotalSeconds - $ready -gt 5) { $forced = $true; $process.Kill(); break }
    if ($clock.Elapsed.TotalSeconds -gt $TimeoutSeconds) { $timedOut = $true; $process.Kill(); break }
    Start-Sleep -Milliseconds 100
    $process.Refresh()
  }
  if (-not $process.WaitForExit(5000)) { throw 'Benchmark process did not exit' }
  $result = $process.ExitCode
} finally {
  if (-not $process.HasExited) { $process.Kill(); [void]$process.WaitForExit(5000) }
}
if (Test-Path -LiteralPath (Join-Path $OutputDir 'summary.json')) {
  [ordered]@{ started_utc = $started; sha256 = $hashes; process_exit_code = $result;
      forced_cleanup_after_report = $forced; timed_out = $timedOut } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDir 'manifest.json') -Encoding UTF8
}
if ($timedOut) { throw "Benchmark exceeded $TimeoutSeconds seconds" }
if (-not (Test-Path -LiteralPath (Join-Path $OutputDir 'summary.json'))) { throw "Benchmark failed before report creation (exit $result)" }
$summary = Get-Content -LiteralPath (Join-Path $OutputDir 'summary.json') -Raw | ConvertFrom-Json
if ($summary.status -ne 'complete' -or ($result -ne 0 -and -not $forced)) { throw "Benchmark incomplete or failed (exit $result); inspect report and input duration." }
if ($forced) { Write-Warning 'Measurements completed; isolated process required forced cleanup after adapter shutdown stalled.' }
Get-Content -LiteralPath (Join-Path $OutputDir 'summary.json') -Raw
