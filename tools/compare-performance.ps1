param(
 [Parameter(Mandatory=$true)][string]$Baseline,
 [Parameter(Mandatory=$true)][string]$Candidate,
 [Parameter(Mandatory=$true)][string]$OutputDir,
 [ValidateRange(0,1000)][double]$ThresholdPercent = 10,
 [switch]$AllowRegression
)
$ErrorActionPreference = 'Stop'
$Baseline = (Resolve-Path -LiteralPath $Baseline).Path
$Candidate = (Resolve-Path -LiteralPath $Candidate).Path
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) { throw 'Use a new comparison directory' }
$a = Get-Content -LiteralPath "$Baseline\summary.json" -Raw | ConvertFrom-Json
$b = Get-Content -LiteralPath "$Candidate\summary.json" -Raw | ConvertFrom-Json
$formatA=if($a.capture_replay_format){$a.capture_replay_format}else{'BGRA'}
$formatB=if($b.capture_replay_format){$b.capture_replay_format}else{'BGRA'}
if($formatA -ne $formatB){throw 'Capture replay formats differ'}
$ma = Get-Content -LiteralPath "$Baseline\manifest.json" -Raw | ConvertFrom-Json
$mb = Get-Content -LiteralPath "$Candidate\manifest.json" -Raw | ConvertFrom-Json
foreach ($s in @($a,$b)) {
 if ($s.schema_version -ne 1 -or $s.status -ne 'complete' -or $s.failures -ne 0 -or
     -not $s.performance_comparable -or $s.capture_output -or $s.measured_frames -le 0) {
  throw 'Requires complete timing-only runs with no failures'
 }
}
if ($ma.timed_out -or $mb.timed_out) { throw 'Cannot compare timed-out runs' }
foreach ($field in @('width','height','fps_numerator','fps_denominator','style','preset','intensity',
 'temporal','warmup_requested','measured_frames','adapter_name','gpu')) {
 if ($a.$field -ne $b.$field) { throw "Incompatible performance metadata: $field" }
}
if ($a.input -eq 'synthetic-v1' -and $b.input -eq 'synthetic-v1') { }
elseif (-not $ma.sha256.input_video -or $ma.sha256.input_video -ne $mb.sha256.input_video) { throw 'Input video hashes differ' }
$ra = @(Import-Csv -LiteralPath "$Baseline\frames.csv" | Where-Object warmup -eq '0')
$rb = @(Import-Csv -LiteralPath "$Candidate\frames.csv" | Where-Object warmup -eq '0')
if ($ra.Count -ne $a.measured_frames -or $rb.Count -ne $ra.Count) { throw 'Frame count differs from summary' }
for ($i=0; $i -lt $ra.Count; $i++) {
 if ($ra[$i].frame -ne $rb[$i].frame -or $ra[$i].timestamp_100ns -ne $rb[$i].timestamp_100ns -or
     $ra[$i].success -ne '1' -or $rb[$i].success -ne '1') { throw 'Frame windows are not aligned or successful' }
}
$metrics = foreach ($field in @('process_mean_us','process_p95_us','process_p99_us')) {
 $before = [double]$a.$field; $after = [double]$b.$field
 if ($before -le 0 -or $after -le 0 -or [double]::IsNaN($before) -or [double]::IsNaN($after) -or
     [double]::IsInfinity($before) -or [double]::IsInfinity($after)) { throw "Invalid metric: $field" }
 $delta = ($after / $before - 1) * 100
 [pscustomobject]@{metric=$field;baseline_us=$before;candidate_us=$after;change_percent=$delta;regressed=($delta -gt $ThresholdPercent)}
}
$passed = @($metrics | Where-Object regressed).Count -eq 0
New-Item -ItemType Directory -Path $OutputDir | Out-Null
[ordered]@{schema_version=1;performance_pass=$passed;threshold_percent=$ThresholdPercent;metrics=@($metrics);
 baseline=$Baseline;candidate=$Candidate;baseline_manifest=$ma;candidate_manifest=$mb;
 note='Offline processing timings, not live FPS or visual quality. Repeat runs to assess noise.'} |
 ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$OutputDir\comparison.json" -Encoding UTF8
$text = @('# Performance comparison','','Offline NR processing time; repeat runs to assess noise.','',
 '| Metric | Baseline us | Candidate us | Change |','| --- | ---: | ---: | ---: |')
foreach ($m in $metrics) { $text += '| {0} | {1:F1} | {2:F1} | {3:F2}% |' -f $m.metric,$m.baseline_us,$m.candidate_us,$m.change_percent }
$text | Set-Content -LiteralPath "$OutputDir\report.md" -Encoding UTF8
if (-not $passed -and -not $AllowRegression) { throw "Performance regression exceeds $ThresholdPercent percent; see $OutputDir\report.md" }
Write-Host "Performance comparison complete; pass=$passed"
