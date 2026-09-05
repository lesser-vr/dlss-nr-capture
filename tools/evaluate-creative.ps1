param(
 [Parameter(Mandatory=$true)][string]$InputVideo,
 [Parameter(Mandatory=$true)][string]$OutputDir,
 [string]$ReleaseDir="$PSScriptRoot\..\build-gpu-flow\Release",
 [ValidateRange(1,10)][int]$Repeats=3,
 [ValidateRange(1,10000)][int]$Frames=300,
 [ValidateRange(0,10000)][int]$Warmup=120,
 [ValidateRange(1,1000)][int]$QualityFrames=180
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $OutputDir){throw 'Use a new evaluation directory'}
if(@(Get-Process dlss-nr-capture,dlss-nr-benchmark -ErrorAction SilentlyContinue).Count){throw 'Close capture and benchmark processes first'}
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$conditions=@(
 @{Name='scale100';NrScale=100;ColorPreserve=0;HighlightGuard=0},
 @{Name='scale75';NrScale=75;ColorPreserve=0;HighlightGuard=0},
 @{Name='scale50';NrScale=50;ColorPreserve=0;HighlightGuard=0},
 @{Name='color100';NrScale=100;ColorPreserve=100;HighlightGuard=0},
 @{Name='highlights';NrScale=100;ColorPreserve=0;HighlightGuard=1}
)
function Run-Condition($condition,[string]$name,[bool]$quality){
 $options=@{InputVideo=$InputVideo;ReleaseDir=$ReleaseDir;OutputDir=(Join-Path $OutputDir $name);
  Warmup=$Warmup;Frames=$Frames;NrScale=$condition.NrScale;ColorPreserve=$condition.ColorPreserve;
  HighlightGuard=$condition.HighlightGuard;GpuCapture=$true}
 if($quality){$options.CaptureOutput=$true;$options.Frames=$QualityFrames;$options.QualityWidth=960;$options.QualityHeight=540}
 & "$PSScriptRoot\benchmark.ps1" @options
 $s=Get-Content -LiteralPath "$($options.OutputDir)\summary.json" -Raw | ConvertFrom-Json
 $m=Get-Content -LiteralPath "$($options.OutputDir)\manifest.json" -Raw | ConvertFrom-Json
 if($s.status -ne 'complete' -or $s.failures -or $m.timed_out -or $m.forced_cleanup_after_report -or $m.process_exit_code -ne 0){throw "Unsuccessful run: $name"}
 Write-Host "$name passed: mean $($s.process_mean_us) us; p95 $($s.process_p95_us) us"
}
# Rotating order reduces, but does not eliminate, thermal/order bias.
for($r=0;$r -lt $Repeats;$r++){
 for($i=0;$i -lt $conditions.Count;$i++){
  $c=$conditions[($i+$r)%$conditions.Count];Run-Condition $c "timing-$($c.Name)-$r" $false
 }
}
foreach($c in $conditions){Run-Condition $c "quality-$($c.Name)" $true}
Run-Condition $conditions[0] 'quality-repeat100' $true
foreach($name in @('scale75','scale50','color100','highlights','repeat100')){
 & "$PSScriptRoot\compare-quality.ps1" -Baseline "$OutputDir\quality-scale100" -Candidate "$OutputDir\quality-$name" -OutputDir "$OutputDir\compare-$name" -ReleaseDir $ReleaseDir -NoVideo
}
Write-Host "Evaluation complete: $OutputDir"
