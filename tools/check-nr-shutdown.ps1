param(
 [string]$ReleaseDir = "$PSScriptRoot/../build/Release",
 [Parameter(Mandatory=$true)][string]$OutputDir,
 [ValidateRange(1,20)][int]$Cycles = 3
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputDir) { throw 'Use a new output directory' }
New-Item -ItemType Directory -Path $OutputDir | Out-Null
for ($cycle = 0; $cycle -lt $Cycles; $cycle++) {
 foreach ($temporal in 0,1) {
  $dest = Join-Path $OutputDir "$cycle-temporal-$temporal"
  & "$PSScriptRoot/benchmark.ps1" -Synthetic -ReleaseDir $ReleaseDir -OutputDir $dest -Warmup 3 -Frames 8 -Temporal $temporal -TimeoutSeconds 30
  $manifest = Get-Content (Join-Path $dest 'manifest.json') -Raw | ConvertFrom-Json
  if ($manifest.forced_cleanup_after_report -or $manifest.timed_out -or $manifest.process_exit_code -ne 0) {
   throw "NR did not shut down cleanly: $dest"
  }
 }
}
Write-Host "NR shutdown checks passed: $($Cycles * 2) processes exited without forced cleanup."
