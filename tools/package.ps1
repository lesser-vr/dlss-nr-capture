param(
 [string]$ReleaseDir = "$PSScriptRoot\..\build\Release",
 [Parameter(Mandatory=$true)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\file-hash.ps1"
$source = (Resolve-Path -LiteralPath $ReleaseDir).Path.TrimEnd('\')
$dest = [IO.Path]::GetFullPath($OutputDir).TrimEnd('\')
if ($dest -eq $source -or $dest.StartsWith($source + '\', [StringComparison]::OrdinalIgnoreCase)) {
 throw 'Package output must be outside the source Release directory'
}
if ((Test-Path -LiteralPath $dest) -or (Test-Path -LiteralPath ($dest + '.zip'))) { throw 'Package destination already exists' }
$files = @('dlss-nr-capture.exe','dlss-nr-worker.exe','dlss-nr-adapter-bridge.dll','dlss-nr-adapter-sample.dll',
 'nr-runtime\dlss5nr_bridge.dll','nr-runtime\caller\nvngx.dll_comfy.dll')
$before = @{}
foreach ($file in $files) { $before[$file] = Get-ArtifactHash (Join-Path $source $file) }
$private = Join-Path $source 'nr-runtime\nvngx_dlssnr.dll'
$privateHash = if (Test-Path -LiteralPath $private) { Get-ArtifactHash $private } else { $null }
New-Item -ItemType Directory -Path $dest | Out-Null
foreach ($file in $files) {
 $target = Join-Path $dest $file
 New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
 Copy-Item -LiteralPath (Join-Path $source $file) -Destination $target
 if ((Get-ArtifactHash $target) -ne $before[$file]) { throw "Package hash mismatch: $file" }
}
Copy-Item -LiteralPath "$PSScriptRoot\..\THIRD_PARTY_NOTICES.md" -Destination $dest
Copy-Item -LiteralPath "$PSScriptRoot\..\third_party\comfyui_dlss5_nr\LICENSE" -Destination (Join-Path $dest 'COMFYUI-DLSS5-NR-LICENSE.txt')
@'
DLSS NR Capture - Windows x64
Visual C++ runtime required. Run dlss-nr-capture.exe.
F10: NR toggle. F11: full screen. View > Refresh devices: rescan inputs.
Audio capture menu: select input and optional audio delay.
Proprietary NR runtime and game test videos are intentionally excluded.
If you have a compatible nvngx_dlssnr.dll, place it in nr-runtime.
Do not distribute that DLL with this package.
Unpack into a new folder; this script never overwrites an existing installation.
'@ | Set-Content -LiteralPath (Join-Path $dest 'PACKAGE-README.txt') -Encoding UTF8
$hashes = [ordered]@{}
Get-ChildItem -LiteralPath $dest -File -Recurse | ForEach-Object {
 $relative = $_.FullName.Substring($dest.Length + 1)
 $hashes[$relative] = Get-ArtifactHash $_.FullName
}
[ordered]@{ schema_version=1; architecture='x64'; proprietary_runtime_included=$false; sha256=$hashes } |
 ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $dest 'manifest.json') -Encoding UTF8
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($dest, $dest + '.zip')
foreach ($file in $files) {
 if ((Get-ArtifactHash (Join-Path $source $file)) -ne $before[$file]) { throw "Source changed: $file" }
}
if ($privateHash -and (Get-ArtifactHash $private) -ne $privateHash) { throw 'Private runtime changed during packaging' }
Write-Host "Package ready: $dest.zip (proprietary runtime excluded; source unchanged)"
