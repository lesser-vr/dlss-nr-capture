param(
 [Parameter(Mandatory=$true)][string]$ReleaseDir,
 [Parameter(Mandatory=$true)][string]$OutputDir,
 [string]$InputVideo
)
# Manual NVIDIA/private-runtime test. Not a CI hardware substitute or a soak test.
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $OutputDir){throw 'Use a new output directory'}
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
$bench=@{ReleaseDir=$ReleaseDir;Frames=24;Warmup=0;CaptureOutput=$true;FullResolution=$true;TimeoutSeconds=120}
if($InputVideo){$bench.InputVideo=$InputVideo}else{$bench.Synthetic=$true}
foreach($mode in @(1,0,2,3)) {
 $path=Join-Path $OutputDir "mode-$mode"
 & "$PSScriptRoot/../tools/benchmark.ps1" @bench -OutputDir $path -TestFlowFailure $mode | Out-Null
 $manifest=Get-Content -LiteralPath "$path/manifest.json" -Raw | ConvertFrom-Json
 if($manifest.forced_cleanup_after_report -or $manifest.process_exit_code -ne 0){throw 'Recovery run did not exit cleanly'}
 $rows=@(Import-Csv -LiteralPath "$path/frames.csv")
 $expected=if($mode){'3'}else{'1'}
 if($rows[-1].flow_mode -ne $expected -or ($mode -and $rows[-1].flow_error -eq '0')){throw "Recovery not exercised: $mode"}
 if($mode -ne 1) {
  $comparison=Join-Path $OutputDir "compare-$mode"
  & "$PSScriptRoot/../tools/compare-quality.ps1" -Baseline "$OutputDir/mode-1" -Candidate $path -OutputDir $comparison -ReleaseDir $ReleaseDir -NoVideo | Out-Null
  $metrics=Get-Content -LiteralPath "$comparison/comparison.json" -Raw | ConvertFrom-Json
  if($metrics.mean_mae -ne 0){throw "Recovery changed NR pixels: $mode"}
 }
 Write-Host "Flow mode $mode passed: correct path, exact output, clean shutdown"
}
