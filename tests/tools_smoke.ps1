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
} finally {
 if ([IO.Path]::GetFullPath($temp).StartsWith([IO.Path]::GetTempPath(), [StringComparison]::OrdinalIgnoreCase)) {
  Remove-Item -LiteralPath $temp -Recurse -Force
 }
}
