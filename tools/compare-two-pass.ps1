param(
 [Parameter(Mandatory=$true)][string]$InputVideo,
 [Parameter(Mandatory=$true)][string]$OutputDir,
 [Parameter(Mandatory=$true)][string]$FfmpegPath,
 [string]$ReleaseDir="$PSScriptRoot\..\build\Release",
 [ValidateRange(1,1000000)][int]$Frames=900,
 [ValidateRange(0,1000000)][int]$Warmup=120
)
$ErrorActionPreference='Stop'
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
if(Test-Path -LiteralPath $OutputDir){throw 'Use a new comparison root'}
$FfmpegPath=(Resolve-Path -LiteralPath $FfmpegPath).Path
$InputVideo=(Resolve-Path -LiteralPath $InputVideo).Path
$ReleaseDir=(Resolve-Path -LiteralPath $ReleaseDir).Path
$priorPath=$env:PATH
try {
 $env:PATH=(Split-Path $FfmpegPath)+';'+$priorPath
 New-Item -ItemType Directory -Path $OutputDir | Out-Null
 foreach($view in 'full','hud') {
  foreach($passes in 1,2) {
   $args=@{InputVideo=$InputVideo;ReleaseDir=$ReleaseDir;OutputDir="$OutputDir/$view-pass$passes";
    NrPasses=$passes;NrScale=100;GpuCapture=$true;CaptureOutput=$true;Warmup=$Warmup;Frames=$Frames}
   if($view -eq 'full'){$args.QualityWidth=960;$args.QualityHeight=540}
   else {$args.FullResolution=$true;$args.RegionX=0;$args.RegionY=0;$args.RegionWidth=640;$args.RegionHeight=360}
   & "$PSScriptRoot/benchmark.ps1" @args
   $manifest=Get-Content "$OutputDir/$view-pass$passes/manifest.json" -Raw | ConvertFrom-Json
   if($manifest.process_exit_code -ne 0 -or $manifest.timed_out -or $manifest.forced_cleanup_after_report){throw 'Quality run did not exit normally'}
  }
  $comparison="$OutputDir/$view-comparison"
  & "$PSScriptRoot/compare-quality.ps1" -Baseline "$OutputDir/$view-pass1" -Candidate "$OutputDir/$view-pass2" -OutputDir $comparison -ReleaseDir $ReleaseDir -BaselineLabel '1 pass - NR 100' -CandidateLabel '2 passes - NR 100'
  $summary=Get-Content "$OutputDir/$view-pass1/summary.json" -Raw | ConvertFrom-Json
  $rate="$($summary.fps_numerator)/$($summary.fps_denominator)"
  & $FfmpegPath -hide_banner -loglevel error -n -i "$comparison/comparison.mp4" -vf 'setpts=2*(PTS-STARTPTS),tpad=stop_mode=clone:stop_duration=1' -r $rate -frames:v ($Frames*2) -c:v libx264 -crf 18 -pix_fmt yuv420p -movflags +faststart "$comparison/half-speed.mp4"
  if($LASTEXITCODE -ne 0){throw 'Half-speed encoding failed'}
  & $FfmpegPath -hide_banner -loglevel error -n -i "$comparison/comparison.mp4" -frames:v 1 "$comparison/first-frame.png"
  if($LASTEXITCODE -ne 0){throw 'Preview extraction failed'}
 }
 Write-Host "Comparison ready: $OutputDir (full / native top-left HUD; silent, same frames; half-speed duplicates frames without optical flow)"
} finally {$env:PATH=$priorPath}
