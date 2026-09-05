param([string]$BenchmarkPath, [string]$AdapterPath, [string]$FfmpegPath, [string]$ComparePath)
$ErrorActionPreference = 'Stop'
$output = Join-Path (Split-Path $BenchmarkPath) ('benchmark-test-' + [guid]::NewGuid().ToString('N'))
& $BenchmarkPath --synthetic --warp --adapter $AdapterPath --output $output --warmup 2 --frames 5 --capture-output
if ($LASTEXITCODE -ne 0) { throw 'Benchmark failed' }
$report = Get-Content -LiteralPath (Join-Path $output 'summary.json') -Raw | ConvertFrom-Json
$rows = @(Import-Csv -LiteralPath (Join-Path $output 'frames.csv'))
if ($report.status -ne 'complete' -or $report.measured_frames -ne 5 -or $report.frames_decoded -ne 7) { throw 'Benchmark frame accounting failed' }
if ($rows.Count -ne 7 -or @($rows | Where-Object warmup -eq '1').Count -ne 2) { throw 'Warmup CSV accounting failed' }
if ($report.live_drop_rate -ne $null -or $report.quality_score -ne $null) { throw 'Unmeasured metrics must be null' }
if (-not $report.capture_output -or $report.performance_comparable) { throw 'Capture must be distinguished from timing benchmark' }
if ((Get-Item -LiteralPath (Join-Path $output 'output.rgb')).Length -ne 5*320*180*3) { throw 'Output frame count mismatch' }
if ([Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $output 'input.rgb'))) -ne
    [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $output 'output.rgb')))) { throw 'Passthrough capture must preserve input RGB pixels' }
$identity = Join-Path $output 'identity-compare'
& $ComparePath $output $output $identity 5 320 180
if ($LASTEXITCODE -ne 0) { throw 'Identity comparison failed' }
$comparison = Get-Content -LiteralPath (Join-Path $identity 'comparison.json') -Raw | ConvertFrom-Json
if ($comparison.mean_mae -ne 0) { throw 'Identical outputs must have zero difference' }
$damaged = Join-Path $output 'damaged'
New-Item -ItemType Directory -Path $damaged | Out-Null
Copy-Item -LiteralPath (Join-Path $output 'input.rgb'),(Join-Path $output 'output.rgb') -Destination $damaged
$stream = [IO.File]::OpenWrite((Join-Path $damaged 'input.rgb'))
try { $stream.WriteByte(255) } finally { $stream.Dispose() }
& $ComparePath $output $damaged (Join-Path $output 'unaligned') 5 320 180
if ($LASTEXITCODE -eq 0) { throw 'Unaligned input must be rejected' }
$original = Get-Content -LiteralPath (Join-Path $output 'summary.json') -Raw
& $BenchmarkPath --synthetic --warp --adapter $AdapterPath --output $output --frames 1 --warmup 0
if ($LASTEXITCODE -eq 0) { throw 'Benchmark overwrote an existing report directory' }
if ((Get-Content -LiteralPath (Join-Path $output 'summary.json') -Raw) -ne $original) { throw 'Existing report changed' }
Write-Host 'benchmark report and overwrite protection checks passed'
 $creative = Join-Path $output 'creative'
 & $BenchmarkPath --synthetic --warp --adapter $AdapterPath --output $creative --warmup 0 --frames 2 --nr-scale 75 --tone 50 --structure 25 --color-preserve 100 --highlight-guard 1
 if ($LASTEXITCODE -ne 0) { throw 'Creative benchmark failed' }
 $c = Get-Content -LiteralPath "$creative\summary.json" -Raw | ConvertFrom-Json
 if ($c.nr_scale -ne 75 -or $c.model_width -ne 960 -or $c.model_height -ne 540 -or $c.tone -ne 50 -or $c.structure -ne 25 -or $c.color_preserve -ne 100 -or $c.highlight_guard -ne 1 -or $c.process_timing_boundary -ne 'prepare-nr-compose-completion-v1') { throw 'Creative benchmark metadata mismatch' }
 & $BenchmarkPath --synthetic --warp --adapter $AdapterPath --output "$output\bad-scale" --nr-scale 60
 if ($LASTEXITCODE -eq 0) { throw 'Invalid NR scale accepted' }
 $full = Join-Path $output 'full-resolution'
 & $BenchmarkPath --synthetic --warp --adapter $AdapterPath --output $full --warmup 0 --frames 1 --capture-output --full-resolution
 if ($LASTEXITCODE -ne 0) { throw 'Full-resolution quality capture failed' }
 if ((Get-Item -LiteralPath "$full\output.rgb").Length -ne 1280*720*3) { throw 'Full-resolution output size incorrect' }
 $region = Join-Path $output 'region'
 & $BenchmarkPath --synthetic --warp --adapter $AdapterPath --output $region --warmup 0 --frames 2 --capture-output --full-resolution --quality-x 5 --quality-y 7 --quality-region-width 17 --quality-region-height 9
 if ($LASTEXITCODE -ne 0) { throw 'ROI quality capture failed' }
 $r = Get-Content -LiteralPath "$region\summary.json" -Raw | ConvertFrom-Json
 if ($r.proxy_width -ne 17 -or $r.proxy_height -ne 9 -or $r.quality_region_x -ne 5 -or $r.quality_region_y -ne 7) { throw 'ROI metadata incorrect' }
 if ((Get-Item -LiteralPath "$region\output.rgb").Length -ne 2*17*9*3) { throw 'ROI output size incorrect' }
 foreach ($dir in @($full,$region)) {
  '{"timed_out":false,"sha256":{}}' | Set-Content -LiteralPath "$dir\manifest.json"
 }
 & "$PSScriptRoot\..\tools\compare-quality.ps1" -Baseline $region -Candidate $region -OutputDir "$region\comparison" -ReleaseDir (Split-Path $ComparePath) -NoVideo
 $r.quality_region_x = 6
 $r | ConvertTo-Json | Set-Content -LiteralPath "$region\summary.json"
 $rejected = $false
 try { & "$PSScriptRoot\..\tools\compare-quality.ps1" -Baseline $full -Candidate $region -OutputDir "$region\invalid" -ReleaseDir (Split-Path $ComparePath) -NoVideo } catch { $rejected = $true }
 if (-not $rejected) { throw 'Mismatched ROI was accepted' }
 Write-Host 'full-resolution capture and ROI identity/metadata checks passed'
$gamingFixture = Join-Path $PSScriptRoot 'gaming test sample vd.mp4'
if (Test-Path -LiteralPath $gamingFixture) {
  & $BenchmarkPath --input $gamingFixture --warp --adapter $AdapterPath --output (Join-Path $output 'gaming-decoder') --warmup 0 --frames 2
  if ($LASTEXITCODE -ne 0) { throw 'Local gaming fixture decoder refresh regression' }
  Write-Host 'local gaming fixture format refresh check passed'
}
if ($FfmpegPath -and (Test-Path -LiteralPath $FfmpegPath)) {
  $fixture = Join-Path $output 'fixture.mp4'
  & $FfmpegPath -hide_banner -loglevel error -n -f lavfi -i 'testsrc2=size=320x240:rate=30' -t 0.2 -c:v libx264 -pix_fmt yuv420p $fixture
  if ($LASTEXITCODE -ne 0) { throw 'Cannot generate MP4 fixture' }
  $fileOutput = Join-Path $output 'file-run'
  & $BenchmarkPath --input $fixture --warp --adapter $AdapterPath --output $fileOutput --warmup 1 --frames 3
  if ($LASTEXITCODE -ne 0) { throw 'File benchmark failed' }
  $fileReport = Get-Content -LiteralPath (Join-Path $fileOutput 'summary.json') -Raw | ConvertFrom-Json
  if ($fileReport.width -ne 320 -or $fileReport.height -ne 240 -or $fileReport.measured_frames -ne 3) { throw 'Decoded file metadata mismatch' }
  $shortOutput = Join-Path $output 'short-file'
  & $BenchmarkPath --input $fixture --warp --adapter $AdapterPath --output $shortOutput --warmup 1 --frames 100
  if ($LASTEXITCODE -ne 2) { throw 'Short file must produce incomplete result' }
  $short = Get-Content -LiteralPath (Join-Path $shortOutput 'summary.json') -Raw | ConvertFrom-Json
  if ($short.status -ne 'incomplete' -or -not $short.eof) { throw 'EOF accounting mismatch' }
  Write-Host 'MP4 decoding and early EOF checks passed'
} else { Write-Host 'SKIP: optional MP4 fixture tests require ffmpeg' }
