param(
  [Parameter(Mandatory=$true)][string]$Baseline,
  [Parameter(Mandatory=$true)][string]$Candidate,
  [Parameter(Mandatory=$true)][string]$OutputDir,
  [string]$ReleaseDir = "$PSScriptRoot\..\build\Release",
  [switch]$NoVideo
)
$ErrorActionPreference = 'Stop'
$Baseline = (Resolve-Path -LiteralPath $Baseline).Path
$Candidate = (Resolve-Path -LiteralPath $Candidate).Path
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) { throw 'Use a new comparison directory' }
$a = Get-Content -LiteralPath (Join-Path $Baseline 'summary.json') -Raw | ConvertFrom-Json
$b = Get-Content -LiteralPath (Join-Path $Candidate 'summary.json') -Raw | ConvertFrom-Json
foreach ($summary in @($a,$b)) {
 foreach ($field in @('quality_region_x','quality_region_y','quality_region_width','quality_region_height')) {
  if ($null -eq $summary.$field) {
   $default = if ($field -eq 'quality_region_width') { $summary.width } elseif ($field -eq 'quality_region_height') { $summary.height } else { 0 }
   $summary | Add-Member -NotePropertyName $field -NotePropertyValue $default -Force
  }
 }
}
foreach ($field in @('quality_region_x','quality_region_y','quality_region_width','quality_region_height')) {
 if ($a.$field -ne $b.$field) { throw "Different quality regions: $field" }
}
if ($a.proxy_format -ne 'rgb24-nearest-v1') { throw 'Unsupported proxy format' }
foreach ($s in @($a,$b)) {
  if ($s.status -ne 'complete' -or -not $s.capture_output) { throw 'Both runs must be complete with -CaptureOutput' }
}
foreach ($field in @('schema_version','width','height','fps_numerator','fps_denominator','style','preset','intensity',
    'temporal','warmup_requested','measured_frames','proxy_format','proxy_width','proxy_height','adapter_name','gpu')) {
  if ($a.$field -ne $b.$field) { throw "Incompatible comparison metadata: $field" }
}
$ma = Get-Content -LiteralPath (Join-Path $Baseline 'manifest.json') -Raw | ConvertFrom-Json
$mb = Get-Content -LiteralPath (Join-Path $Candidate 'manifest.json') -Raw | ConvertFrom-Json
if ($ma.timed_out -or $mb.timed_out) { throw 'Cannot compare timed-out runs' }
if ($a.input -eq 'synthetic-v1' -and $b.input -eq 'synthetic-v1') { }
elseif (-not $ma.sha256.input_video -or $ma.sha256.input_video -ne $mb.sha256.input_video) { throw 'Input video hashes differ' }
$rowsA = @(Import-Csv -LiteralPath (Join-Path $Baseline 'frames.csv') | Where-Object warmup -eq '0')
$rowsB = @(Import-Csv -LiteralPath (Join-Path $Candidate 'frames.csv') | Where-Object warmup -eq '0')
if ($rowsA.Count -ne $a.measured_frames -or $rowsB.Count -ne $rowsA.Count) { throw 'Frame metadata count mismatch' }
for ($i=0; $i -lt $rowsA.Count; $i++) {
  if ($rowsA[$i].timestamp_100ns -ne $rowsB[$i].timestamp_100ns -or $rowsA[$i].frame -ne $rowsB[$i].frame -or
      $rowsA[$i].success -ne '1' -or $rowsB[$i].success -ne '1') { throw 'Frame timestamps or success flags differ' }
}
& (Join-Path $ReleaseDir 'dlss-nr-quality-compare.exe') $Baseline $Candidate $OutputDir $a.measured_frames $a.proxy_width $a.proxy_height
if ($LASTEXITCODE -ne 0) { throw 'Pixel comparison failed; inspect input alignment/archive sizes' }
$metrics = @(Import-Csv -LiteralPath (Join-Path $OutputDir 'comparison.csv'))
$top = @($metrics | Sort-Object { [double]$_.baseline_candidate_mae + [Math]::Max(0, [double]$_.residual_increase) } -Descending | Select-Object -First 20)
$table = foreach ($row in $top) {
  $i = [int]$row.index
  $time = [double]$rowsB[$i].timestamp_100ns / 10000000.0
  '<tr><td>{0}</td><td>{1:F3}</td><td>{2}</td><td>{3}</td><td>{4}</td></tr>' -f $rowsB[$i].frame,$time,$row.baseline_candidate_mae,$row.residual_increase,$row.review_flag
}
$video = ''
$ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
if (-not $NoVideo -and $ffmpeg) {
  $size = "$($a.proxy_width)x$($a.proxy_height)"
  $rate = "$($a.fps_numerator)/$($a.fps_denominator)"
  $videoArgs = @('-hide_banner','-loglevel','error','-n')
  foreach ($file in @((Join-Path $Baseline 'input.rgb'),(Join-Path $Baseline 'output.rgb'),(Join-Path $Candidate 'output.rgb'))) {
    $videoArgs += @('-f','rawvideo','-pixel_format','rgb24','-video_size',$size,'-framerate',$rate,'-i',$file)
  }
  $videoArgs += @('-filter_complex','[0:v][1:v][2:v]hstack=inputs=3,pad=ceil(iw/2)*2:ceil(ih/2)*2','-c:v','libx264','-crf','18','-pix_fmt','yuv420p','-movflags','+faststart',(Join-Path $OutputDir 'comparison.mp4'))
  & $ffmpeg.Source @videoArgs
  if ($LASTEXITCODE -ne 0) { throw 'Comparison video encoding failed; numeric report is available' }
  $video = '<video controls width="960" src="comparison.mp4"></video>'
}
$html = @"
<!doctype html><meta charset="utf-8"><title>NR comparison</title>
<style>body{font:16px system-ui;margin:32px;max-width:1100px}table{border-collapse:collapse}td,th{border:1px solid #ccc;padding:8px}video{max-width:100%}</style>
<h1>NR output comparison</h1>
<p>Video columns: source / baseline / candidate. Preview starts after warmup; table times refer to the original source.</p>
$video
<p>$($a.proxy_width)x$($a.proxy_height) RGB samples from source region ($($a.quality_region_x), $($a.quality_region_y), $($a.quality_region_width), $($a.quality_region_height)). Scores use RGB levels 0-255. No motion compensation; flags are review hints, NOT flicker/ghosting verdicts. MP4 is a lossy preview; metrics use raw samples.</p>
<p>Review rules: frame MAE &gt; 5, temporal residual increase &gt; 3, or nearly static input (change &lt; 1) with candidate residual change &gt; 3. No automatic quality pass/fail.</p>
<h2>Largest changes (up to 20 frames, including unflagged frames)</h2>
<table><tr><th>Source frame</th><th>Source seconds</th><th>Output MAE</th><th>Residual increase</th><th>Review flag</th></tr>$($table -join "`n")</table>
"@
$html | Set-Content -LiteralPath (Join-Path $OutputDir 'report.html') -Encoding UTF8
[ordered]@{baseline=$Baseline;candidate=$Candidate;baseline_manifest=$ma;candidate_manifest=$mb} |
  ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDir 'provenance.json') -Encoding UTF8
Get-Content -LiteralPath (Join-Path $OutputDir 'comparison.json') -Raw
Write-Host "Report: $OutputDir\report.html"
